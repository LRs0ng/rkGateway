#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/control.hpp"

#include "camera_capture.h"

#include <chrono>
#include <condition_variable>
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

    CameraDriver(
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
        camera_control_settings_t controls);
    ~CameraDriver() override;

    [[nodiscard]] DriverCapabilities capabilities() const override;
    void configure(const DeviceConfig& device, SampleSink sink) override;
    void start() override;
    void stop() noexcept override;
    [[nodiscard]] DeviceControlResult control(
        const DeviceControlRequest& request) override;

private:
    [[nodiscard]] ByteArray capture_one();
    [[nodiscard]] EnqueueResult emit_frame(ByteArray&& image);
    void run(std::stop_token stop_token) noexcept;

    std::string device_path_;
    unsigned int width_;
    unsigned int height_;
    camera_pixel_format_t pixel_format_;
    Mode mode_;
    std::chrono::milliseconds interval_;
    unsigned int warmup_frames_;
    int capture_timeout_ms_;
    std::string control_command_;
    std::string control_device_;
    camera_control_settings_t controls_{};

    DeviceConfig device_;
    SampleSink sink_;
    camera_capture_t capture_{};
    std::jthread worker_;
    std::mutex capture_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable_any wakeup_;
    bool configured_{false};
    bool started_{false};
};

}  // namespace gateway
