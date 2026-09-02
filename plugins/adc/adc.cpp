#include "adc.hpp"

#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace gateway {
namespace {

constexpr std::string_view kPluginName{"Pico ADC push source"};
constexpr std::string_view kDefaultSerialDevice{"/dev/ttyACM0"};
constexpr std::string_view kFrameMagic{"ADC1"};
constexpr std::uint8_t kProtocolVersion{1};
constexpr std::uint8_t kSampleFormatU16Le{1};
constexpr std::size_t kHeaderSize{36U};
constexpr std::size_t kCrcOffset{32U};
constexpr std::size_t kMaxBatchSize{100000U};
constexpr std::uint32_t kCounterModulo{1000000000U};

std::string optional_string(
    const plugin_json::Json& settings,
    std::string_view key,
    std::string default_value) {
    const auto item = settings.find(key);
    if (item == settings.end()) {
        return default_value;
    }
    if (!item->is_string() || item->get<std::string>().empty()) {
        plugin_json::fail(kPluginName, key, "must be a non-empty string");
    }
    return item->get<std::string>();
}

std::uint32_t optional_u32(
    const plugin_json::Json& settings,
    std::string_view key,
    std::uint32_t default_value,
    bool positive = false) {
    const auto item = settings.find(key);
    if (item == settings.end()) {
        return default_value;
    }
    std::uint64_t parsed = 0U;
    if (item->is_number_unsigned()) {
        parsed = item->get<std::uint64_t>();
    } else if (item->is_number_integer()) {
        const auto signed_value = item->get<std::int64_t>();
        if (signed_value < 0) {
            plugin_json::fail(
                kPluginName, key,
                positive ? "must be positive"
                         : "must be a non-negative integer");
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        plugin_json::fail(
            kPluginName, key,
            positive ? "must be positive"
                     : "must be a non-negative integer");
    }
    if (parsed > std::numeric_limits<std::uint32_t>::max() ||
        (positive && parsed == 0U)) {
        plugin_json::fail(
            kPluginName, key, positive ? "must be positive" : "is too large");
    }
    return static_cast<std::uint32_t>(parsed);
}

double optional_number(
    const plugin_json::Json& settings,
    std::string_view key,
    double default_value) {
    const auto item = settings.find(key);
    if (item == settings.end()) {
        return default_value;
    }
    if (!item->is_number()) {
        plugin_json::fail(kPluginName, key, "must be a number");
    }
    const auto value = item->get<double>();
    if (!std::isfinite(value)) {
        plugin_json::fail(kPluginName, key, "must be finite");
    }
    return value;
}

std::chrono::milliseconds optional_timeout(
    const plugin_json::Json& settings,
    std::string_view key,
    std::chrono::milliseconds default_value) {
    const auto value = optional_u32(
        settings, key, static_cast<std::uint32_t>(default_value.count()), true);
    if (value > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        plugin_json::fail(kPluginName, key, "is too large");
    }
    return std::chrono::milliseconds{static_cast<int>(value)};
}

std::uint16_t get_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8U));
}

std::uint32_t get_u32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::int64_t sample_time_ns(
    std::int64_t received_time_ns,
    std::size_t index,
    std::uint32_t sample_rate_hz,
    std::size_t count) {
    const auto rate = static_cast<std::int64_t>(sample_rate_hz);
    // A batch containing N samples spans N-1 sampling intervals.  Using N
    // here would shift the reconstructed first timestamp by one sample.
    const auto intervals = count > 0U ? count - 1U : 0U;
    const auto total_ns = static_cast<std::int64_t>(intervals) * 1000000000LL;
    const auto first_offset_ns = total_ns / rate;
    const auto current_offset_ns =
        static_cast<std::int64_t>(index) * 1000000000LL / rate;
    return received_time_ns - first_offset_ns + current_offset_ns;
}

std::unique_ptr<AdcPicoDriver> make_adc(std::string_view settings_json) {
    const auto settings = plugin_json::parse_object(settings_json, kPluginName);
    const auto value_mode = optional_string(settings, "value_mode", "voltage");
    AdcPicoDriver::ValueMode mode;
    if (value_mode == "voltage") {
        mode = AdcPicoDriver::ValueMode::Voltage;
    } else if (value_mode == "raw") {
        mode = AdcPicoDriver::ValueMode::Raw;
    } else {
        plugin_json::fail(kPluginName, "value_mode", "must be voltage or raw");
    }

    const auto vref = optional_number(settings, "vref", 3.3);
    if (vref <= 0.0) {
        plugin_json::fail(kPluginName, "vref", "must be greater than zero");
    }
    return std::make_unique<AdcPicoDriver>(
        optional_string(settings, "device", std::string{kDefaultSerialDevice}),
        optional_string(settings, "point", "voltage"),
        optional_u32(settings, "sample_rate_hz", 0U),
        optional_u32(settings, "batch_size", 0U),
        mode,
        vref,
        optional_timeout(settings, "read_timeout_ms", std::chrono::milliseconds{100}));
}

}  // namespace

AdcPicoDriver::AdcPicoDriver(
    std::string serial_device,
    std::string point,
    unsigned expected_sample_rate_hz,
    unsigned expected_batch_size,
    ValueMode value_mode,
    double vref,
    std::chrono::milliseconds read_timeout)
    : serial_device_(std::move(serial_device)),
      point_(std::move(point)),
      expected_sample_rate_hz_(expected_sample_rate_hz),
      expected_batch_size_(expected_batch_size),
      value_mode_(value_mode),
      vref_(vref),
      read_timeout_(read_timeout),
      input_buffer_() {
    if (serial_device_.empty() || point_.empty()) {
        throw std::invalid_argument("ADC serial device and point are required");
    }
    if (expected_batch_size_ > kMaxBatchSize) {
        throw std::invalid_argument("ADC batch size is too large");
    }
    if (vref_ <= 0.0 || !std::isfinite(vref_)) {
        throw std::invalid_argument("ADC vref must be finite and positive");
    }
    if (read_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("ADC read timeout must be positive");
    }
    input_buffer_.reserve(kHeaderSize + 1000U);
}

AdcPicoDriver::~AdcPicoDriver() {
    stop();
}

DriverCapabilities AdcPicoDriver::capabilities() const {
    return DriverCapabilities{.mode = AcquisitionMode::Push};
}

void AdcPicoDriver::configure(const DeviceConfig& device, SampleSink sink) {
    if (started_.load(std::memory_order_acquire)) {
        throw std::logic_error("cannot configure a running ADC driver");
    }
    if (!sink) {
        throw std::invalid_argument("ADC push source requires a sample sink");
    }
    if (device.points.size() != 1U || device.points.front().name != point_) {
        throw std::invalid_argument(
            "ADC device must contain exactly one point named '" + point_ + "'");
    }
    if (device.points.front().type != ValueType::Double &&
        device.points.front().type != ValueType::Integer) {
        throw std::invalid_argument(
            "ADC point must use value type double or integer: " + point_);
    }
    device_ = device;
    sink_ = std::move(sink);
    configured_.store(true, std::memory_order_release);
}

bool AdcPicoDriver::open_serial() {
    fd_ = ::open(
        serial_device_.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }

    struct termios settings {};
    if (::tcgetattr(fd_, &settings) == 0) {
        ::cfmakeraw(&settings);
        ::cfsetispeed(&settings, B115200);
        ::cfsetospeed(&settings, B115200);
        settings.c_cflag |= static_cast<tcflag_t>(CLOCAL | CREAD);
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 0;
        (void)::tcsetattr(fd_, TCSANOW, &settings);
    }
    return true;
}

void AdcPicoDriver::close_serial() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

void AdcPicoDriver::start() {
    if (!configured_.load(std::memory_order_acquire)) {
        throw std::logic_error("ADC driver is not configured");
    }
    if (started_.exchange(true, std::memory_order_acq_rel)) {
        throw std::logic_error("ADC driver is already started");
    }
    if (!open_serial()) {
        started_.store(false, std::memory_order_release);
        throw std::runtime_error(
            "cannot open Pico ADC serial device " + serial_device_ + ": " +
            std::strerror(errno));
    }
    input_buffer_.clear();
    try {
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
    } catch (...) {
        started_.store(false, std::memory_order_release);
        close_serial();
        throw;
    }
}

void AdcPicoDriver::stop() noexcept {
    started_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    close_serial();
    input_buffer_.clear();
}

std::uint32_t AdcPicoDriver::crc32_with_zeroed_field(
    const std::uint8_t* data,
    std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        const auto byte =
            (index >= kCrcOffset && index < kCrcOffset + 4U) ? 0U : data[index];
        crc ^= byte;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                ? (crc >> 1U) ^ 0xEDB88320U
                : (crc >> 1U);
        }
    }
    return ~crc;
}

void AdcPicoDriver::consume_bytes(
    const std::uint8_t* data,
    std::size_t size) {
    input_buffer_.insert(input_buffer_.end(), data, data + size);
    constexpr std::size_t max_buffer_size = kHeaderSize + kMaxBatchSize * 2U;
    if (input_buffer_.size() > max_buffer_size + kHeaderSize) {
        const auto position = std::search(
            input_buffer_.end() - static_cast<std::ptrdiff_t>(kHeaderSize),
            input_buffer_.end(), kFrameMagic.begin(), kFrameMagic.end());
        if (position == input_buffer_.end()) {
            input_buffer_.erase(
                input_buffer_.begin(),
                input_buffer_.end() - static_cast<std::ptrdiff_t>(kHeaderSize - 1U));
        } else {
            input_buffer_.erase(input_buffer_.begin(), position);
        }
    }

    Frame frame;
    while (true) {
        const auto before = input_buffer_.size();
        if (try_decode_frame(frame)) {
            emit_frame(frame);
        }
        // Invalid data is discarded by try_decode_frame and should not be
        // emitted. Stop only when a complete frame is not yet available.
        if (input_buffer_.size() == before) {
            break;
        }
    }
}

bool AdcPicoDriver::try_decode_frame(Frame& frame) {
    const auto magic = std::search(
        input_buffer_.begin(), input_buffer_.end(),
        kFrameMagic.begin(), kFrameMagic.end());
    if (magic == input_buffer_.end()) {
        const auto keep = std::min<std::size_t>(input_buffer_.size(), 3U);
        if (input_buffer_.size() > keep) {
            input_buffer_.erase(
                input_buffer_.begin(),
                input_buffer_.end() - static_cast<std::ptrdiff_t>(keep));
        }
        return false;
    }
    if (magic != input_buffer_.begin()) {
        input_buffer_.erase(input_buffer_.begin(), magic);
    }
    if (input_buffer_.size() < kHeaderSize) {
        return false;
    }

    const auto* bytes = input_buffer_.data();
    const auto version = bytes[4];
    const auto format = bytes[5];
    const auto header_size = get_u16(bytes + 6U);
    const auto sample_rate = get_u32(bytes + 24U);
    const auto sample_count = get_u32(bytes + 28U);
    const auto expected_crc = get_u32(bytes + kCrcOffset);
    const auto header_valid =
        version == kProtocolVersion && format == kSampleFormatU16Le &&
        header_size == kHeaderSize && sample_rate > 0U &&
        sample_count > 0U && sample_count <= kMaxBatchSize &&
        (expected_sample_rate_hz_ == 0U ||
         sample_rate == expected_sample_rate_hz_) &&
        (expected_batch_size_ == 0U ||
         sample_count == expected_batch_size_);
    if (!header_valid) {
        input_buffer_.erase(input_buffer_.begin());
        return false;
    }

    const auto payload_size = static_cast<std::size_t>(sample_count) * 2U;
    const auto frame_size = kHeaderSize + payload_size;
    if (input_buffer_.size() < frame_size) {
        return false;
    }
    if (crc32_with_zeroed_field(bytes, frame_size) != expected_crc) {
        input_buffer_.erase(input_buffer_.begin());
        return false;
    }

    frame.session_id = get_u32(bytes + 8U);
    frame.sequence = get_u32(bytes + 12U);
    frame.first_sample = get_u32(bytes + 16U);
    frame.dropped_total = get_u32(bytes + 20U);
    frame.sample_rate_hz = sample_rate;
    frame.samples.resize(sample_count);
    for (std::size_t index = 0; index < sample_count; ++index) {
        frame.samples[index] = get_u16(bytes + kHeaderSize + index * 2U);
    }
    input_buffer_.erase(
        input_buffer_.begin(),
        input_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    return true;
}

void AdcPicoDriver::emit_frame(const Frame& frame) {
    if (!sink_) {
        return;
    }
    const auto received_time_ns = unix_time_ns();
    RawBatch batch{
        .device_id = device_.id,
        .source = "adc_pico",
        .samples = {},
    };
    batch.samples.reserve(frame.samples.size());
    for (std::size_t index = 0; index < frame.samples.size(); ++index) {
        const auto raw = static_cast<double>(frame.samples[index]);
        const auto value = value_mode_ == ValueMode::Voltage
            ? raw * vref_ / 65535.0
            : raw;
        batch.samples.push_back(RawSample{
            .point = point_,
            .value = value,
            .status = Quality::Good,
            .source_time_ns = sample_time_ns(
                received_time_ns, index, frame.sample_rate_hz,
                frame.samples.size()),
        });
    }
    const auto result = sink_(std::move(batch));
    if (result == EnqueueResult::Full) {
        std::cerr << "[adc] raw queue full; dropping ADC batch sequence "
                  << frame.sequence << '\n';
    } else if (result == EnqueueResult::Stopping) {
        started_.store(false, std::memory_order_release);
    }
}

void AdcPicoDriver::run(std::stop_token token) noexcept {
    std::vector<std::uint8_t> read_buffer(8192U);
    while (!token.stop_requested() && started_.load(std::memory_order_acquire)) {
        struct pollfd descriptor {
            .fd = fd_,
            .events = POLLIN,
            .revents = 0,
        };
        const auto timeout = static_cast<int>(std::min<std::int64_t>(
            read_timeout_.count(), 100));
        const auto poll_result = ::poll(&descriptor, 1, timeout);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[adc] poll failed: " << std::strerror(errno) << '\n';
            break;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            std::cerr << "[adc] serial device error\n";
            break;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) {
            continue;
        }
        const auto count = ::read(fd_, read_buffer.data(), read_buffer.size());
        if (count > 0) {
            consume_bytes(read_buffer.data(), static_cast<std::size_t>(count));
        } else if (count < 0 && errno != EAGAIN && errno != EINTR) {
            std::cerr << "[adc] read failed: " << std::strerror(errno) << '\n';
            break;
        } else if (count == 0) {
            std::cerr << "[adc] Pico serial device disconnected\n";
            break;
        }
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_adc(
            settings_json == nullptr ? std::string_view{"{}"}
                                      : std::string_view{settings_json})
            .release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<AdcPicoDriver*>(plugin);
}

}  // namespace gateway
