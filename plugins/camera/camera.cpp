#include "camera.hpp"

#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <utility>

namespace gateway {
namespace {

constexpr std::string_view kPluginName{"camera push source"};

const plugin_json::Json* optional_member(
    const plugin_json::Json& settings,
    std::string_view key) {
    const auto item = settings.find(key);
    return item == settings.end() ? nullptr : &*item;
}

std::string optional_string(
    const plugin_json::Json& settings,
    std::string_view key,
    std::string default_value) {
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->is_string() || value->get<std::string>().empty()) {
        plugin_json::fail(kPluginName, key, "must be a non-empty string");
    }
    return value->get<std::string>();
}

unsigned int optional_unsigned(
    const plugin_json::Json& settings,
    std::string_view key,
    unsigned int default_value,
    bool positive = false) {
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    std::uint64_t parsed = 0U;
    if (value->is_number_unsigned()) {
        parsed = value->get<std::uint64_t>();
    } else if (value->is_number_integer()) {
        const auto signed_value = value->get<std::int64_t>();
        if (signed_value < 0) {
            plugin_json::fail(kPluginName, key,
                              positive ? "must be positive"
                                       : "must be a non-negative integer");
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        plugin_json::fail(kPluginName, key,
                          positive ? "must be positive"
                                   : "must be a non-negative integer");
    }
    if (parsed > static_cast<std::uint64_t>(
                     std::numeric_limits<unsigned int>::max()) ||
        (positive && parsed == 0U)) {
        plugin_json::fail(kPluginName, key,
                          positive ? "must be positive" : "is too large");
    }
    return static_cast<unsigned int>(parsed);
}

int optional_signed(
    const plugin_json::Json& settings,
    std::string_view key,
    int default_value) {
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    std::int64_t parsed = 0;
    if (value->is_number_integer()) {
        parsed = value->get<std::int64_t>();
    } else if (value->is_number_unsigned()) {
        const auto unsigned_value = value->get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(
                                 std::numeric_limits<int>::max())) {
            plugin_json::fail(kPluginName, key, "is too large");
        }
        parsed = static_cast<std::int64_t>(unsigned_value);
    } else {
        plugin_json::fail(kPluginName, key, "must be an integer");
    }
    if (parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        plugin_json::fail(kPluginName, key, "is out of range");
    }
    return static_cast<int>(parsed);
}

int optional_timeout(
    const plugin_json::Json& settings,
    std::string_view key,
    int default_value) {
    const auto value = optional_unsigned(settings, key,
                                         static_cast<unsigned int>(default_value),
                                         true);
    if (value > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
        plugin_json::fail(kPluginName, key, "is too large");
    }
    return static_cast<int>(value);
}

CameraDriver::Mode parse_mode(const plugin_json::Json& settings) {
    const auto mode = optional_string(settings, "mode", "periodic");
    if (mode == "periodic") {
        return CameraDriver::Mode::Periodic;
    }
    if (mode == "control") {
        return CameraDriver::Mode::Control;
    }
    plugin_json::fail(kPluginName, "mode", "must be periodic or control");
}

camera_pixel_format_t parse_pixel_format(const plugin_json::Json& settings) {
    const auto value = optional_string(settings, "pixfmt", "auto");
    camera_pixel_format_t format = CAMERA_PIXFMT_AUTO;
    if (camera_pixel_format_parse(value.c_str(), &format) < 0) {
        plugin_json::fail(kPluginName, "pixfmt",
                          "must be auto, mjpeg, nv12, yuyv, yuv420 or rgb24");
    }
    return format;
}

std::unique_ptr<CameraDriver> make_camera(std::string_view settings_json) {
    const auto settings = plugin_json::parse_object(settings_json, kPluginName);
    const auto mode = parse_mode(settings);
    const auto interval_ms = optional_unsigned(settings, "interval_ms", 10000U,
                                               mode == CameraDriver::Mode::Periodic);
    const auto timeout_ms = optional_timeout(settings, "capture_timeout_ms", 3000);
    camera_control_settings_t controls{};
    if (optional_member(settings, "brightness") != nullptr) {
        controls.has_brightness = 1;
        controls.brightness = optional_signed(settings, "brightness", 0);
    }
    if (optional_member(settings, "exposure") != nullptr) {
        controls.has_exposure = 1;
        controls.exposure = optional_signed(settings, "exposure", 0);
    }
    if (optional_member(settings, "analogue_gain") != nullptr) {
        controls.has_analogue_gain = 1;
        controls.analogue_gain = optional_signed(settings, "analogue_gain", 0);
    }
    return std::make_unique<CameraDriver>(
        optional_string(settings, "device", "/dev/video0"),
        optional_unsigned(settings, "width", 1920U, true),
        optional_unsigned(settings, "height", 1080U, true),
        parse_pixel_format(settings),
        mode,
        std::chrono::milliseconds{interval_ms},
        optional_unsigned(settings, "warmup_frames", 3U),
        timeout_ms,
        optional_string(settings, "control_command", "capture"),
        optional_string(settings, "control_device", "auto"),
        controls);
}

}  // namespace

CameraDriver::CameraDriver(
    std::string device_path,
    unsigned int width,
    unsigned int height,
    camera_pixel_format_t pixel_format,
    Mode mode,
    std::chrono::milliseconds interval,
    unsigned int warmup_frames,
    int capture_timeout_ms,
    std::string control_command,
    std::string control_device,
    camera_control_settings_t controls)
    : device_path_(std::move(device_path)),
      width_(width),
      height_(height),
      pixel_format_(pixel_format),
      mode_(mode),
      interval_(interval),
      warmup_frames_(warmup_frames),
      capture_timeout_ms_(capture_timeout_ms),
      control_command_(std::move(control_command)),
      control_device_(std::move(control_device)),
      controls_(controls) {
    capture_.fd = -1;
    capture_.control_fd = -1;
    if (device_path_.empty() || width_ == 0U || height_ == 0U ||
        interval_ <= std::chrono::milliseconds::zero() ||
        capture_timeout_ms_ <= 0 || control_command_.empty() ||
        control_device_.empty()) {
        throw std::invalid_argument("invalid camera driver settings");
    }
}

CameraDriver::~CameraDriver() {
    stop();
}

DriverCapabilities CameraDriver::capabilities() const {
    return DriverCapabilities{.mode = AcquisitionMode::Push};
}

void CameraDriver::configure(const DeviceConfig& device, SampleSink sink) {
    if (started_) {
        throw std::logic_error("cannot configure a running camera driver");
    }
    if (!sink) {
        throw std::invalid_argument("camera push source requires a sample sink");
    }
    if (device.points.empty()) {
        throw std::invalid_argument("camera device must define at least one point");
    }
    for (const auto& point : device.points) {
        if (point.type != ValueType::ByteArray) {
            throw std::invalid_argument(
                "camera points must use bytes/byte_array type: " + point.name);
        }
    }
    device_ = device;
    sink_ = std::move(sink);
    configured_ = true;
}

void CameraDriver::start() {
    if (!configured_) {
        throw std::logic_error("camera driver is not configured");
    }
    if (started_) {
        throw std::logic_error("camera driver is already started");
    }
    {
        std::lock_guard lock(capture_mutex_);
        if (camera_capture_init(&capture_, device_path_.c_str(), width_, height_,
                                pixel_format_, warmup_frames_,
                                capture_timeout_ms_, control_device_.c_str(),
                                &controls_) < 0) {
            throw std::runtime_error(
                "cannot start camera " + device_path_ + ": " +
                std::strerror(errno));
        }
    }
    started_ = true;
    if (mode_ == Mode::Periodic) {
        worker_ = std::jthread(
            [this](std::stop_token token) { run(token); });
    }
}

void CameraDriver::stop() noexcept {
    if (worker_.joinable()) {
        worker_.request_stop();
        wakeup_.notify_all();
        worker_.join();
    }
    std::lock_guard lock(capture_mutex_);
    camera_capture_close(&capture_);
    started_ = false;
}

ByteArray CameraDriver::capture_one() {
    camera_frame_t frame{};
    uint8_t* image = nullptr;
    size_t image_size = 0U;
    uint32_t image_format = 0U;
    if (camera_capture_read(&capture_, &frame) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "capture frame");
    }
    if (camera_frame_to_image(&frame, &image, &image_size, &image_format) < 0) {
        const int saved = errno;
        camera_frame_release(&frame);
        throw std::system_error(saved, std::generic_category(),
                                "convert camera frame");
    }
    (void)image_format;
    ByteArray result(image, image + image_size);
    free(image);
    camera_frame_release(&frame);
    return result;
}

EnqueueResult CameraDriver::emit_frame(ByteArray&& image) {
    RawBatch batch{
        .device_id = device_.id,
        .source = "camera",
        .samples = {},
    };
    const auto timestamp = unix_time_ns();
    batch.samples.reserve(device_.points.size());
    for (const auto& point : device_.points) {
        batch.samples.push_back(RawSample{
            .point = point.name,
            .value = image,
            .status = Quality::Good,
            .source_time_ns = timestamp,
        });
    }
    return sink_(std::move(batch));
}

void CameraDriver::run(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        try {
            ByteArray image;
            {
                std::lock_guard lock(capture_mutex_);
                image = capture_one();
            }
            if (emit_frame(std::move(image)) == EnqueueResult::Stopping) {
                return;
            }
        } catch (const std::exception& error) {
            std::cerr << "[camera] capture failed: " << error.what() << '\n';
        }
        std::unique_lock lock(wait_mutex_);
        if (wakeup_.wait_for(lock, interval_,
                             [&stop_token] { return stop_token.stop_requested(); })) {
            return;
        }
    }
}

DeviceControlResult CameraDriver::control(
    const DeviceControlRequest& request) {
    auto failed = [&request](DeviceControlStatus status, std::string message) {
        return DeviceControlResult{
            .request_id = request.request_id,
            .status = status,
            .outputs = {},
            .message = std::move(message),
        };
    };
    if (request.device_id != device_.id) {
        return failed(DeviceControlStatus::InvalidArgument,
                      "control request belongs to another device");
    }
    if (request.command != control_command_) {
        return failed(DeviceControlStatus::Unsupported,
                      "unsupported camera command: " + request.command);
    }
    if (!started_) {
        return failed(DeviceControlStatus::Failed, "camera is stopped");
    }
    if (ControlClock::now() >= request.deadline) {
        return failed(DeviceControlStatus::Timeout, "camera control deadline expired");
    }
    try {
        ByteArray image;
        {
            std::lock_guard lock(capture_mutex_);
            image = capture_one();
        }
        if (ControlClock::now() >= request.deadline) {
            return failed(DeviceControlStatus::Timeout,
                          "camera capture exceeded control deadline");
        }
        const auto enqueue_result = emit_frame(std::move(image));
        if (enqueue_result == EnqueueResult::Stopping) {
            return failed(DeviceControlStatus::Cancelled,
                          "sample sink is stopping");
        }
        return DeviceControlResult{
            .request_id = request.request_id,
            .status = DeviceControlStatus::Succeeded,
            .outputs = {},
            .message = "camera frame captured and pushed",
        };
    } catch (const std::system_error& error) {
        return failed(DeviceControlStatus::Failed,
                      std::string{"camera capture failed: "} + error.what());
    } catch (const std::exception& error) {
        return failed(DeviceControlStatus::Failed, error.what());
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_camera(
                   settings_json == nullptr ? std::string_view{"{}"}
                                             : std::string_view{settings_json})
            .release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<CameraDriver*>(plugin);
}

}  // namespace gateway
