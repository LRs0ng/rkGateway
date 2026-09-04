#pragma once

#include "gateway/event_publisher.hpp"
#include "screen_fb.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gateway {

class ScreenEventPublisher final : public IEventPublisher {
public:
    ScreenEventPublisher(std::string settings_json);
    ~ScreenEventPublisher() override;

    void configure(const GatewayConfig& gateway) override;
    void start() override;
    [[nodiscard]] EventPublishResult publish(const Event& event) override;
    void stop() noexcept override;

private:
    std::string settings_json_;
    std::string device_;
    std::string image_point_;
    std::string image_format_;
    std::string pixel_format_;
    std::string output_mode_;
    unsigned int rotation_degrees_{0};
    unsigned int image_width_{1080};
    unsigned int image_height_{1920};
    unsigned int image_target_width_{0};
    unsigned int image_target_height_{0};
    unsigned int expected_width_{1080};
    unsigned int expected_height_{1920};
    bool fit_image_{true};
    unsigned int numeric_scale_{1};
    int numeric_x_{0};
    int numeric_y_{0};
    uint32_t numeric_foreground_{0xffffffU};
    uint32_t numeric_background_{0x000000U};
    bool clear_before_numeric_{true};
    bool wait_for_vsync_{true};
    bool show_camera_frame_stats_{false};
    std::string camera_device_id_;
    unsigned int frame_stats_scale_{2};
    unsigned int frame_stats_margin_x_{16};
    unsigned int frame_stats_margin_y_{16};
    uint32_t frame_stats_foreground_{0x00ff00U};
    uint32_t frame_stats_background_{0x000000U};

    std::uint64_t camera_frame_count_{0};
    double camera_fps_{0.0};
    std::chrono::steady_clock::time_point last_camera_frame_at_{};
    screen_fb_t* screen_{nullptr};
    std::mutex mutex_;
    bool configured_{false};
    bool started_{false};
};

}  // namespace gateway
