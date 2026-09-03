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

bool optional_bool(
    const plugin_json::Json& settings,
    std::string_view key,
    bool default_value) {
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->is_boolean()) {
        plugin_json::fail(kPluginName, key, "must be a boolean");
    }
    return value->get<bool>();
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

CameraDriver::SnapshotPolicy parse_snapshot_policy(
    const plugin_json::Json& settings) {
    const auto policy = optional_string(settings, "snapshot_policy", "latest");
    if (policy == "latest") {
        return CameraDriver::SnapshotPolicy::Latest;
    }
    if (policy == "next") {
        return CameraDriver::SnapshotPolicy::Next;
    }
    plugin_json::fail(kPluginName, "snapshot_policy", "must be latest or next");
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
        optional_string(settings, "sensor_device", "auto"),
        optional_unsigned(settings, "sensor_width", 1944U, true),
        optional_unsigned(settings, "sensor_height", 1096U, true),
        optional_unsigned(settings, "crop_width", 1080U, true),
        optional_unsigned(settings, "crop_height", 1080U, true),
        optional_unsigned(settings, "width", 640U, true),
        optional_unsigned(settings, "height", 640U, true),
        parse_pixel_format(settings),
        optional_bool(settings, "rga_rgb24", true),
        mode,
        optional_bool(settings, "initial_capture", false),
        std::chrono::milliseconds{interval_ms},
        optional_unsigned(settings, "warmup_frames", 3U),
        timeout_ms,
        parse_snapshot_policy(settings),
        std::chrono::milliseconds{optional_unsigned(
            settings, "snapshot_wait_ms", 1000U, true)},
        std::chrono::milliseconds{optional_unsigned(
            settings, "max_frame_age_ms", 500U)},
        optional_string(settings, "control_command", "capture"),
        optional_string(settings, "control_device", "auto"),
        controls);
}

}  // namespace

CameraDriver::CameraDriver(
    std::string device_path,
    std::string sensor_device,
    unsigned int sensor_width,
    unsigned int sensor_height,
    unsigned int crop_width,
    unsigned int crop_height,
    unsigned int width,
    unsigned int height,
    camera_pixel_format_t pixel_format,
    bool rga_rgb24,
    Mode mode,
    bool initial_capture,
    std::chrono::milliseconds interval,
    unsigned int warmup_frames,
    int capture_timeout_ms,
    SnapshotPolicy snapshot_policy,
    std::chrono::milliseconds snapshot_wait,
    std::chrono::milliseconds max_frame_age,
    std::string control_command,
    std::string control_device,
    camera_control_settings_t controls)
    : device_path_(std::move(device_path)),
      sensor_device_(std::move(sensor_device)),
      sensor_width_(sensor_width),
      sensor_height_(sensor_height),
      crop_width_(crop_width),
      crop_height_(crop_height),
      width_(width),
      height_(height),
      pixel_format_(pixel_format),
      rga_rgb24_(rga_rgb24),
      mode_(mode),
      initial_capture_(initial_capture),
      interval_(interval),
      warmup_frames_(warmup_frames),
      capture_timeout_ms_(capture_timeout_ms),
      snapshot_policy_(snapshot_policy),
      snapshot_wait_(snapshot_wait),
      max_frame_age_(max_frame_age),
      control_command_(std::move(control_command)),
      control_device_(std::move(control_device)),
      controls_(controls) {
    capture_.fd = -1;
    capture_.sensor_fd = -1;
    capture_.control_fd = -1;
    capture_.cancel_fd = -1;
    if (device_path_.empty() || sensor_device_.empty() ||
        sensor_width_ == 0U || sensor_height_ == 0U ||
        crop_width_ == 0U || crop_height_ == 0U ||
        crop_width_ > sensor_width_ || crop_height_ > sensor_height_ ||
        width_ == 0U || height_ == 0U ||
        (rga_rgb24_ && pixel_format_ != CAMERA_PIXFMT_AUTO &&
         pixel_format_ != CAMERA_PIXFMT_NV12) ||
        interval_ <= std::chrono::milliseconds::zero() ||
        capture_timeout_ms_ <= 0 ||
        snapshot_wait_ <= std::chrono::milliseconds::zero() ||
        max_frame_age_ < std::chrono::milliseconds::zero() ||
        control_command_.empty() || control_device_.empty()) {
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
    if (started_.load(std::memory_order_acquire)) {
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
    if (started_.load(std::memory_order_acquire)) {
        throw std::logic_error("camera driver is already started");
    }

    {
        std::lock_guard lock(latest_mutex_);
        camera_frame_release(&latest_frame_);
        latest_sequence_ = 0U;
        latest_source_time_ns_ = 0;
        latest_received_at_ = {};
        latest_error_.clear();
    }
    const camera_pipeline_settings_t pipeline{
        .sensor_device = sensor_device_.c_str(),
        .sensor_width = sensor_width_,
        .sensor_height = sensor_height_,
        .crop_width = crop_width_,
        .crop_height = crop_height_,
        .rga_rgb24 = rga_rgb24_ ? 1 : 0,
    };
    if (camera_capture_init_ex(
            &capture_, device_path_.c_str(), width_, height_, pixel_format_,
            warmup_frames_, capture_timeout_ms_, control_device_.c_str(),
            &controls_, &pipeline) < 0) {
        throw std::runtime_error(
            "cannot start camera " + device_path_ + ": " +
            std::strerror(errno));
    }
    std::cerr << "[camera] sensor=" << sensor_device_
              << " mode=" << sensor_width_ << "x" << sensor_height_
              << " crop=" << capture_.crop_left << "," << capture_.crop_top
              << "/" << capture_.crop_width << "x" << capture_.crop_height
              << " rkisp_output=" << capture_.width << "x" << capture_.height
              << " " << camera_pixel_format_name(capture_.pixfmt)
              << " frame_output=" << (rga_rgb24_ ? "RGB24(RGA)" : "native")
              << '\n';

    started_.store(true, std::memory_order_release);
    try {
        capture_worker_ = std::jthread(
            [this](std::stop_token token) { capture_loop(token); });
        if (mode_ == Mode::Periodic) {
            publish_worker_ = std::jthread(
                [this](std::stop_token token) { publish_loop(token); });
        }
    } catch (...) {
        stop();
        throw;
    }
}

void CameraDriver::stop() noexcept {
    const bool was_started = started_.exchange(false, std::memory_order_acq_rel);
    if (publish_worker_.joinable()) {
        publish_worker_.request_stop();
    }
    if (capture_worker_.joinable()) {
        capture_worker_.request_stop();
    }
    wakeup_.notify_all();
    latest_ready_.notify_all();
    if (was_started || capture_.fd >= 0) {
        camera_capture_cancel(&capture_);
    }
    if (publish_worker_.joinable()) {
        publish_worker_.join();
    }
    if (capture_worker_.joinable()) {
        capture_worker_.join();
    }
    camera_capture_close(&capture_);
    {
        std::lock_guard lock(latest_mutex_);
        camera_frame_release(&latest_frame_);
        latest_sequence_ = 0U;
        latest_source_time_ns_ = 0;
        latest_received_at_ = {};
        latest_error_.clear();
    }
}

void CameraDriver::capture_loop(std::stop_token stop_token) noexcept {
    camera_frame_t incoming{};
    bool initial_frame_published = false;
    while (!stop_token.stop_requested()) {
        if (camera_capture_read_reuse(&capture_, &incoming) < 0) {
            const int saved = errno;
            if (stop_token.stop_requested() || saved == ECANCELED) {
                break;
            }
            {
                std::lock_guard lock(latest_mutex_);
                latest_error_ = std::strerror(saved);
            }
            latest_ready_.notify_all();
            std::cerr << "[camera] streaming capture failed: "
                      << std::strerror(saved) << " (" << saved << ")\n";
            std::unique_lock retry_lock(wait_mutex_);
            if (wakeup_.wait_for(
                    retry_lock, std::chrono::milliseconds{100},
                    [&stop_token] { return stop_token.stop_requested(); })) {
                break;
            }
            continue;
        }

        const auto received_at = ControlClock::now();
        const auto source_time_ns = unix_time_ns();
        {
            std::lock_guard lock(latest_mutex_);
            std::swap(latest_frame_, incoming);
            ++latest_sequence_;
            latest_source_time_ns_ = source_time_ns;
            latest_received_at_ = received_at;
            latest_error_.clear();
        }
        latest_ready_.notify_all();

        // Controlled mode normally publishes only after a control request.
        // Publish exactly one frame after the stream has produced a valid
        // image so a processor (for example YOLO) has an event that can start
        // the request/response capture loop.
        if (mode_ == Mode::Control && initial_capture_ &&
            !initial_frame_published) {
            try {
                auto image = capture_snapshot(
                    ControlClock::time_point::max(), true);
                const auto enqueue_result = emit_frame(std::move(image));
                if (enqueue_result == EnqueueResult::Stopping) {
                    break;
                }
                if (enqueue_result == EnqueueResult::Accepted) {
                    initial_frame_published = true;
                } else {
                    std::cerr << "[camera] initial snapshot event queue is full; "
                                 "will retry\n";
                }
            } catch (const std::exception& error) {
                if (!stop_token.stop_requested()) {
                    std::cerr << "[camera] initial snapshot failed: "
                              << error.what() << '\n';
                }
            }
        }
    }
    camera_frame_release(&incoming);
    latest_ready_.notify_all();
}

CameraDriver::CapturedImage CameraDriver::capture_snapshot(
    ControlClock::time_point deadline,
    bool force_latest) {
    const auto local_deadline = ControlClock::now() + snapshot_wait_;
    if (deadline > local_deadline) {
        deadline = local_deadline;
    }

    camera_frame_t frame{};
    ByteArray raw_frame;
    std::uint64_t sequence = 0U;
    std::int64_t source_time_ns = 0;
    {
        std::unique_lock lock(latest_mutex_);
        const std::uint64_t baseline = latest_sequence_;
        const auto ready = [this, baseline, force_latest] {
            if (!started_.load(std::memory_order_acquire) ||
                latest_frame_.data == nullptr || latest_frame_.size == 0U) {
                return !started_.load(std::memory_order_acquire);
            }
            if (!force_latest && snapshot_policy_ == SnapshotPolicy::Next &&
                latest_sequence_ <= baseline) {
                return false;
            }
            if (max_frame_age_ > std::chrono::milliseconds::zero() &&
                ControlClock::now() - latest_received_at_ > max_frame_age_) {
                return false;
            }
            return true;
        };

        if (!ready() && !latest_ready_.wait_until(lock, deadline, ready)) {
            const std::string detail = latest_error_.empty()
                ? std::string{}
                : std::string{"; last streaming error: "} + latest_error_;
            throw std::system_error(
                std::error_code(ETIMEDOUT, std::generic_category()),
                "wait for camera snapshot" + detail);
        }
        if (!started_.load(std::memory_order_acquire)) {
            throw std::system_error(ECANCELED, std::generic_category(),
                                    "camera stopped while waiting for snapshot");
        }

        frame = latest_frame_;
        raw_frame.assign(latest_frame_.data,
                         latest_frame_.data + latest_frame_.size);
        frame.data = raw_frame.data();
        frame.capacity = raw_frame.size();
        sequence = latest_sequence_;
        source_time_ns = latest_source_time_ns_;
    }

    if (frame.pixfmt == V4L2_PIX_FMT_MJPEG ||
        frame.pixfmt == V4L2_PIX_FMT_RGB24) {
        return CapturedImage{
            .bytes = std::move(raw_frame),
            .source_time_ns = source_time_ns,
            .sequence = sequence,
        };
    }

    size_t image_capacity = 0U;
    size_t image_size = 0U;
    uint32_t image_format = 0U;
    if (camera_frame_image_capacity(&frame, &image_capacity) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "size camera snapshot");
    }
    ByteArray result(image_capacity);
    if (camera_frame_to_image_buffer(&frame, result.data(), result.size(),
                                     &image_size, &image_format) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "convert camera snapshot");
    }
    (void)image_format;
    result.resize(image_size);
    return CapturedImage{
        .bytes = std::move(result),
        .source_time_ns = source_time_ns,
        .sequence = sequence,
    };
}

EnqueueResult CameraDriver::emit_frame(CapturedImage&& image) {
    RawBatch batch{
        .device_id = device_.id,
        .source = "camera",
        .samples = {},
    };
    batch.samples.reserve(device_.points.size());
    for (std::size_t index = 0U; index < device_.points.size(); ++index) {
        ByteArray value = index + 1U == device_.points.size()
            ? std::move(image.bytes)
            : image.bytes;
        batch.samples.push_back(RawSample{
            .point = device_.points[index].name,
            .value = std::move(value),
            .status = Quality::Good,
            .source_time_ns = image.source_time_ns,
        });
    }
    return sink_(std::move(batch));
}

void CameraDriver::publish_loop(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        try {
            auto image = capture_snapshot(ControlClock::time_point::max());
            if (emit_frame(std::move(image)) == EnqueueResult::Stopping) {
                return;
            }
        } catch (const std::system_error& error) {
            if (stop_token.stop_requested() || error.code().value() == ECANCELED) {
                return;
            }
            std::cerr << "[camera] snapshot failed: " << error.what() << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[camera] snapshot failed: " << error.what() << '\n';
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
    if (!started_.load(std::memory_order_acquire)) {
        return failed(DeviceControlStatus::Failed, "camera is stopped");
    }
    if (ControlClock::now() >= request.deadline) {
        return failed(DeviceControlStatus::Timeout,
                      "camera control deadline expired");
    }

    try {
        auto image = capture_snapshot(request.deadline);
        const auto sequence = image.sequence;
        if (ControlClock::now() >= request.deadline) {
            return failed(DeviceControlStatus::Timeout,
                          "camera snapshot conversion exceeded control deadline");
        }
        const auto enqueue_result = emit_frame(std::move(image));
        if (enqueue_result == EnqueueResult::Stopping) {
            return failed(DeviceControlStatus::Cancelled,
                          "sample sink is stopping");
        }
        return DeviceControlResult{
            .request_id = request.request_id,
            .status = DeviceControlStatus::Succeeded,
            .outputs = {{"frame_sequence", static_cast<std::int64_t>(sequence)}},
            .message = "latest camera frame pushed",
        };
    } catch (const std::system_error& error) {
        if (error.code().value() == ETIMEDOUT) {
            return failed(DeviceControlStatus::Timeout, error.what());
        }
        if (error.code().value() == ECANCELED) {
            return failed(DeviceControlStatus::Cancelled, error.what());
        }
        return failed(DeviceControlStatus::Failed,
                      std::string{"camera snapshot failed: "} + error.what());
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
