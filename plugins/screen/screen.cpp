#include "screen.hpp"

#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>

namespace gateway {
namespace {

constexpr std::string_view kPluginName{"screen event publisher"};
using Json = plugin_json::Json;

const Json* optional_member(const Json& settings, std::string_view key)
{
    const auto item = settings.find(key);
    return item == settings.end() ? nullptr : &*item;
}

std::string optional_string(const Json& settings, std::string_view key,
                           std::string default_value)
{
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->is_string() || value->get<std::string>().empty()) {
        plugin_json::fail(kPluginName, key, "must be a non-empty string");
    }
    return value->get<std::string>();
}

unsigned int optional_unsigned(const Json& settings, std::string_view key,
                               unsigned int default_value, bool positive = false)
{
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

int optional_int(const Json& settings, std::string_view key, int default_value)
{
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->is_number_integer()) {
        plugin_json::fail(kPluginName, key, "must be an integer");
    }
    const auto parsed = value->get<std::int64_t>();
    if (parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        plugin_json::fail(kPluginName, key, "is out of range");
    }
    return static_cast<int>(parsed);
}

bool optional_bool(const Json& settings, std::string_view key, bool default_value)
{
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->is_boolean()) {
        plugin_json::fail(kPluginName, key, "must be a boolean");
    }
    return value->get<bool>();
}

uint32_t optional_color(const Json& settings, std::string_view key,
                        uint32_t default_value)
{
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    std::uint64_t parsed = 0U;
    if (value->is_number_unsigned()) {
        parsed = value->get<std::uint64_t>();
    } else if (value->is_string()) {
        const auto text = value->get<std::string>();
        std::size_t position = 0U;
        try {
            parsed = std::stoull(text, &position, 0);
        } catch (...) {
            plugin_json::fail(kPluginName, key,
                              "must be an RGB888 integer or 0xRRGGBB string");
        }
        if (position != text.size()) {
            plugin_json::fail(kPluginName, key,
                              "must be an RGB888 integer or 0xRRGGBB string");
        }
    } else {
        plugin_json::fail(kPluginName, key,
                          "must be an RGB888 integer or 0xRRGGBB string");
    }
    if (parsed > 0xffffffU) {
        plugin_json::fail(kPluginName, key, "must be in the range 0x000000..0xffffff");
    }
    return static_cast<uint32_t>(parsed);
}

}  // namespace

ScreenEventPublisher::ScreenEventPublisher(std::string settings_json)
    : settings_json_(std::move(settings_json))
{
    const auto settings = plugin_json::parse_object(settings_json_, kPluginName);
    device_ = optional_string(settings, "device", "/dev/fb0");
    image_point_ = optional_string(settings, "image_point", "image");
    image_format_ = optional_string(settings, "image_format", "auto");
    if (image_format_ != "auto" && image_format_ != "ppm" &&
        image_format_ != "rgb888" && image_format_ != "rgb24" &&
        image_format_ != "rgb565") {
        plugin_json::fail(kPluginName, "image_format",
                          "must be auto, ppm, rgb888, rgb24 or rgb565");
    }
    pixel_format_ = optional_string(settings, "pixel_format", "rgb888");
    if (pixel_format_ != "rgb888") {
        plugin_json::fail(kPluginName, "pixel_format",
                          "must be rgb888 for the HX8399-C screen");
    }
    output_mode_ = optional_string(settings, "output_mode", "portrait");
    if (output_mode_ != "portrait" && output_mode_ != "landscape") {
        plugin_json::fail(kPluginName, "output_mode",
                          "must be portrait or landscape");
    }
    const unsigned int default_rotation = output_mode_ == "landscape" ? 90U : 0U;
    rotation_degrees_ = optional_unsigned(settings, "rotation_degrees",
                                          default_rotation);
    if (rotation_degrees_ != 0U && rotation_degrees_ != 90U &&
        rotation_degrees_ != 180U && rotation_degrees_ != 270U) {
        plugin_json::fail(kPluginName, "rotation_degrees",
                          "must be 0, 90, 180 or 270 degrees");
    }
    const bool rotated_quarter_turn = rotation_degrees_ == 90U ||
                                      rotation_degrees_ == 270U;
    image_width_ = optional_unsigned(settings, "image_width",
                                     rotated_quarter_turn ? 1920U : 1080U,
                                     true);
    image_height_ = optional_unsigned(settings, "image_height",
                                      rotated_quarter_turn ? 1080U : 1920U,
                                      true);
    expected_width_ = optional_unsigned(settings, "expected_width", 1080U, true);
    expected_height_ = optional_unsigned(settings, "expected_height", 1920U, true);
    fit_image_ = optional_bool(settings, "fit_image", true);
    numeric_scale_ = optional_unsigned(settings, "numeric_scale", 1U, true);
    numeric_x_ = optional_int(settings, "numeric_x", 0);
    numeric_y_ = optional_int(settings, "numeric_y", 0);
    numeric_foreground_ = optional_color(settings, "numeric_foreground", 0xffffffU);
    numeric_background_ = optional_color(settings, "numeric_background", 0x000000U);
    clear_before_numeric_ = optional_bool(settings, "clear_before_numeric", true);
}

ScreenEventPublisher::~ScreenEventPublisher()
{
    stop();
}

void ScreenEventPublisher::configure(const GatewayConfig& gateway)
{
    std::lock_guard lock(mutex_);
    if (started_) {
        throw std::logic_error("cannot configure a running screen publisher");
    }
    for (const auto& device : gateway.devices) {
        (void)device;
    }
    if (image_format_ == "rgb565" || image_format_ == "rgb888" ||
        image_format_ == "rgb24") {
        if (image_width_ == 0U || image_height_ == 0U) {
            throw std::invalid_argument(
                "screen publisher raw image_format requires image_width and image_height");
        }
    }
    configured_ = true;
}

void ScreenEventPublisher::start()
{
    std::lock_guard lock(mutex_);
    if (!configured_) {
        throw std::logic_error("screen publisher is not configured");
    }
    if (started_) {
        throw std::logic_error("screen publisher is already running");
    }
    screen_ = screen_fb_open(device_.c_str());
    if (screen_ == nullptr) {
        throw std::system_error(errno, std::generic_category(),
                                "open framebuffer " + device_);
    }
    if (screen_fb_width(screen_) != expected_width_ ||
        screen_fb_height(screen_) != expected_height_) {
        const auto actual_width = screen_fb_width(screen_);
        const auto actual_height = screen_fb_height(screen_);
        screen_fb_close(screen_);
        screen_ = nullptr;
        throw std::invalid_argument(
            "framebuffer geometry is " + std::to_string(actual_width) + "x" +
            std::to_string(actual_height) + ", expected " +
            std::to_string(expected_width_) + "x" +
            std::to_string(expected_height_));
    }
    if (!screen_fb_is_rgb888(screen_)) {
        screen_fb_close(screen_);
        screen_ = nullptr;
        throw std::invalid_argument(
            "framebuffer is not RGB888 (red/green/blue channels must be 8-bit)");
    }
    if (screen_fb_clear(screen_, 0x000000U) < 0) {
        const int saved = errno;
        screen_fb_close(screen_);
        screen_ = nullptr;
        throw std::system_error(saved, std::generic_category(),
                                "clear framebuffer " + device_);
    }
    started_ = true;
    std::cerr << "screen publisher: device=" << device_
              << " size=" << screen_fb_width(screen_) << "x"
              << screen_fb_height(screen_) << " bpp=" << screen_fb_bpp(screen_)
              << " pixel_format=RGB888 panel=HX8399-C touch=GT911 dsi=DSI0"
              << " mode=" << output_mode_ << " rotation=" << rotation_degrees_
              << " logical_size="
              << ((rotation_degrees_ == 90U || rotation_degrees_ == 270U)
                      ? screen_fb_height(screen_) : screen_fb_width(screen_))
              << "x"
              << ((rotation_degrees_ == 90U || rotation_degrees_ == 270U)
                      ? screen_fb_width(screen_) : screen_fb_height(screen_))
              << "\n";
}

EventPublishResult ScreenEventPublisher::publish(const Event& event)
{
    std::lock_guard lock(mutex_);
    if (!started_ || screen_ == nullptr) {
        return EventPublishResult::Unavailable;
    }

    bool displayed_image = false;
    const Reading* selected_image = find_reading(event, image_point_);
    if (selected_image == nullptr) {
        for (const auto& reading : event.readings) {
            if (std::holds_alternative<ByteArray>(reading.value)) {
                selected_image = &reading;
                break;
            }
        }
    }
    if (selected_image != nullptr && selected_image->quality == Quality::Good &&
        std::holds_alternative<ByteArray>(selected_image->value)) {
        const auto& bytes = std::get<ByteArray>(selected_image->value);
        if (screen_fb_present_rotated(
                screen_, bytes.data(), bytes.size(), image_format_.c_str(),
                image_width_, image_height_, fit_image_ ? 1 : 0, 0x000000U,
                rotation_degrees_) < 0) {
            std::cerr << "screen publisher: display image failed: "
                      << std::strerror(errno) << "\n";
            return EventPublishResult::Rejected;
        }
        displayed_image = true;
    }

    int numeric_y = numeric_y_;
    bool drew_number = false;
    for (const auto& reading : event.readings) {
        const auto numeric = numeric_value(reading.value);
        if (reading.quality != Quality::Good || !numeric ||
            !std::isfinite(*numeric)) {
            continue;
        }
        if (screen_fb_draw_number_rotated(
                screen_, *numeric, numeric_x_, numeric_y, numeric_scale_,
                numeric_foreground_, numeric_background_,
                (clear_before_numeric_ && !drew_number) ? 1 : 0,
                rotation_degrees_) < 0) {
            std::cerr << "screen publisher: draw number failed: "
                      << std::strerror(errno) << "\n";
            return EventPublishResult::Rejected;
        }
        drew_number = true;
        numeric_y += static_cast<int>(10U * numeric_scale_);
    }
    if (!displayed_image && !drew_number) {
        return EventPublishResult::Accepted;
    }
    return EventPublishResult::Accepted;
}

void ScreenEventPublisher::stop() noexcept
{
    std::lock_guard lock(mutex_);
    if (screen_ != nullptr) {
        screen_fb_close(screen_);
        screen_ = nullptr;
    }
    started_ = false;
}

}  // namespace gateway

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(const char* settings_json)
{
    try {
        return new gateway::ScreenEventPublisher(
            settings_json == nullptr ? "{}" : settings_json);
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin)
{
    delete static_cast<gateway::ScreenEventPublisher*>(plugin);
}
