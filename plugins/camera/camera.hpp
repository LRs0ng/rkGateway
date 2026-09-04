#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/control.hpp"

#include "camera_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace gateway {

class CameraDriver final : public IProtocolDriver {
public:
    enum class Mode {
        Periodic,
        Control,
    };

    enum class SnapshotPolicy {
        Latest,
        Next,
    };

    CameraDriver(
        std::string device_path,
        unsigned int sensor_mode_width,
        unsigned int sensor_mode_height,
        unsigned int sensor_active_width,
        unsigned int sensor_active_height,
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
        camera_control_settings_t controls);
    ~CameraDriver() override;

    [[nodiscard]] DriverCapabilities capabilities() const override;
    void configure(const DeviceConfig& device, SampleSink sink) override;
    void start() override;
    void stop() noexcept override;
    [[nodiscard]] DeviceControlResult control(
        const DeviceControlRequest& request) override;

private:
    struct CapturedImage {
        ByteArray bytes;
        std::int64_t source_time_ns{0};
        std::uint64_t sequence{0};
    };

    [[nodiscard]] CapturedImage capture_snapshot(
        ControlClock::time_point deadline,
        bool force_latest = false);
    [[nodiscard]] EnqueueResult emit_frame(CapturedImage&& image);
    void capture_loop(std::stop_token stop_token) noexcept;
    void publish_loop(std::stop_token stop_token) noexcept;

    std::string device_path_;
    unsigned int sensor_mode_width_;
    unsigned int sensor_mode_height_;
    unsigned int sensor_active_width_;
    unsigned int sensor_active_height_;
    unsigned int crop_width_;
    unsigned int crop_height_;
    unsigned int width_;
    unsigned int height_;
    camera_pixel_format_t pixel_format_;
    bool rga_rgb24_;
    Mode mode_;
    bool initial_capture_;
    std::chrono::milliseconds interval_;
    unsigned int warmup_frames_;
    int capture_timeout_ms_;
    SnapshotPolicy snapshot_policy_;
    std::chrono::milliseconds snapshot_wait_;
    std::chrono::milliseconds max_frame_age_;
    std::string control_command_;
    std::string control_device_;
    camera_control_settings_t controls_{};

    DeviceConfig device_;
    SampleSink sink_;
    camera_capture_t capture_{};
    std::jthread capture_worker_;
    std::jthread publish_worker_;

    std::mutex latest_mutex_;
    std::condition_variable latest_ready_;
    camera_frame_t latest_frame_{};
    std::uint64_t latest_sequence_{0};
    std::int64_t latest_source_time_ns_{0};
    ControlClock::time_point latest_received_at_{};
    std::string latest_error_;

    std::mutex wait_mutex_;
    std::condition_variable_any wakeup_;
    bool configured_{false};
    std::atomic_bool started_{false};
};

}  // namespace gateway
