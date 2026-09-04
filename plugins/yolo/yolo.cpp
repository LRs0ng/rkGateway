#include "yolo.hpp"

#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace gateway {
namespace {

constexpr std::string_view kPluginName{"yolov5 npu processor"};
constexpr unsigned int kClassCount = 80U;
constexpr unsigned int kAttributesPerAnchor = 5U + kClassCount;
constexpr unsigned int kAnchorCount = 3U;

constexpr std::array<std::array<int, 6>, 3> kAnchors{{
    {{10, 13, 16, 30, 33, 23}},
    {{30, 61, 62, 45, 59, 119}},
    {{116, 90, 156, 198, 373, 326}},
}};

constexpr std::array<const char*, kClassCount> kCocoLabels{{
    "person", "bicycle", "car", "motorcycle", "airplane", "bus",
    "train", "truck", "boat", "traffic light", "fire hydrant",
    "stop sign", "parking meter", "bench", "bird", "cat", "dog",
    "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe",
    "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat",
    "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
    "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana",
    "apple", "sandwich", "orange", "broccoli", "carrot", "hot dog",
    "pizza", "donut", "cake", "chair", "couch", "potted plant", "bed",
    "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard",
    "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator",
    "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
    "toothbrush",
}};

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
                                       : "must be non-negative");
        }
        parsed = static_cast<std::uint64_t>(signed_value);
    } else {
        plugin_json::fail(kPluginName, key, "must be an integer");
    }
    if (parsed > static_cast<std::uint64_t>(
                     std::numeric_limits<unsigned int>::max()) ||
        (positive && parsed == 0U)) {
        plugin_json::fail(kPluginName, key,
                          positive ? "must be positive" : "is too large");
    }
    return static_cast<unsigned int>(parsed);
}

float optional_float(
    const plugin_json::Json& settings,
    std::string_view key,
    float default_value) {
    const auto* value = optional_member(settings, key);
    if (value == nullptr || !value->is_number()) {
        if (value != nullptr) {
            plugin_json::fail(kPluginName, key, "must be a number");
        }
        return default_value;
    }
    const double parsed = value->get<double>();
    if (!std::isfinite(parsed)) {
        plugin_json::fail(kPluginName, key, "must be finite");
    }
    return static_cast<float>(parsed);
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
        plugin_json::fail(kPluginName, key, "must be boolean");
    }
    return value->get<bool>();
}

std::uint32_t parse_color(
    const plugin_json::Json& settings,
    std::string_view key,
    std::uint32_t default_value) {
    const auto* value = optional_member(settings, key);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->is_string()) {
        plugin_json::fail(kPluginName, key, "must be a string such as 0xff0000");
    }
    const auto text = value->get<std::string>();
    std::size_t offset = 0U;
    try {
        const auto result = std::stoul(text, &offset, 0);
        if (offset != text.size() || result > 0x00ffffffUL) {
            throw std::invalid_argument("invalid color");
        }
        return static_cast<std::uint32_t>(result);
    } catch (...) {
        plugin_json::fail(kPluginName, key,
                          "must be a 24-bit color string such as 0xff0000");
    }
}

std::uint64_t tensor_elements(const rknn_tensor_attr& attr) {
    if (attr.n_dims <= 0 || attr.n_dims > RKNN_MAX_DIMS) {
        throw std::runtime_error("YOLO tensor has an invalid rank");
    }
    std::uint64_t result = 1U;
    for (uint32_t index = 0U; index < attr.n_dims; ++index) {
        if (attr.dims[index] == 0U ||
            result > std::numeric_limits<std::uint64_t>::max() /
                         attr.dims[index]) {
            throw std::runtime_error("YOLO tensor dimensions overflow");
        }
        result *= attr.dims[index];
    }
    return result;
}

std::string tensor_shape(const rknn_tensor_attr& attr) {
    std::string text{"["};
    for (uint32_t index = 0U; index < attr.n_dims; ++index) {
        if (index != 0U) {
            text += ",";
        }
        text += std::to_string(attr.dims[index]);
    }
    text += "]";
    return text;
}

float sigmoid(float value) {
    if (value >= 0.0F) {
        const float z = std::exp(-value);
        return 1.0F / (1.0F + z);
    }
    const float z = std::exp(value);
    return z / (1.0F + z);
}

float overlap(const YoloProcessor::Detection& left,
              const YoloProcessor::Detection& right) {
    const int x1 = std::max(left.left, right.left);
    const int y1 = std::max(left.top, right.top);
    const int x2 = std::min(left.right, right.right);
    const int y2 = std::min(left.bottom, right.bottom);
    const int width = std::max(0, x2 - x1 + 1);
    const int height = std::max(0, y2 - y1 + 1);
    const int intersection = width * height;
    const int area_left = std::max(0, left.right - left.left + 1) *
                          std::max(0, left.bottom - left.top + 1);
    const int area_right = std::max(0, right.right - right.left + 1) *
                           std::max(0, right.bottom - right.top + 1);
    const int uni = area_left + area_right - intersection;
    return uni <= 0 ? 0.0F : static_cast<float>(intersection) /
                                static_cast<float>(uni);
}

void put_pixel(ByteArray& image, unsigned int width, unsigned int height,
               int x, int y, std::uint32_t color) {
    if (x < 0 || y < 0 || static_cast<unsigned int>(x) >= width ||
        static_cast<unsigned int>(y) >= height) {
        return;
    }
    const auto offset = (static_cast<std::size_t>(y) * width +
                         static_cast<unsigned int>(x)) * 3U;
    image[offset + 0U] = static_cast<std::uint8_t>((color >> 16U) & 0xffU);
    image[offset + 1U] = static_cast<std::uint8_t>((color >> 8U) & 0xffU);
    image[offset + 2U] = static_cast<std::uint8_t>(color & 0xffU);
}

void draw_rect(ByteArray& image, unsigned int width, unsigned int height,
               const YoloProcessor::Detection& detection, std::uint32_t color,
               unsigned int thickness) {
    for (unsigned int offset = 0U; offset < thickness; ++offset) {
        const int inset = static_cast<int>(offset);
        for (int x = detection.left; x <= detection.right; ++x) {
            put_pixel(image, width, height, x, detection.top + inset, color);
            put_pixel(image, width, height, x, detection.bottom - inset, color);
        }
        for (int y = detection.top; y <= detection.bottom; ++y) {
            put_pixel(image, width, height, detection.left + inset, y, color);
            put_pixel(image, width, height, detection.right - inset, y, color);
        }
    }
}

// A compact 5x7 font is enough for the ASCII COCO class names. Lowercase
// letters are normalized to uppercase before drawing.
std::array<std::uint8_t, 7> glyph(char character) {
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- .%";
    const std::array<std::array<std::uint8_t, 7>, 40> font{{
        {{0x1e,0x05,0x05,0x1e,0x14,0x14,0x14}}, // A
        {{0x1f,0x14,0x14,0x1f,0x14,0x14,0x1f}}, // B
        {{0x0f,0x10,0x10,0x10,0x10,0x10,0x0f}}, // C
        {{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}}, // D
        {{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}}, // E
        {{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}}, // F
        {{0x0f,0x10,0x10,0x17,0x11,0x11,0x0f}}, // G
        {{0x11,0x11,0x11,0x1f,0x11,0x11,0x11}}, // H
        {{0x1f,0x04,0x04,0x04,0x04,0x04,0x1f}}, // I
        {{0x01,0x01,0x01,0x01,0x11,0x11,0x0e}}, // J
        {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, // K
        {{0x10,0x10,0x10,0x10,0x10,0x10,0x1f}}, // L
        {{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}}, // M
        {{0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, // N
        {{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}}, // O
        {{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}}, // P
        {{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}}, // Q
        {{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}}, // R
        {{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}}, // S
        {{0x1f,0x04,0x04,0x04,0x04,0x04,0x04}}, // T
        {{0x11,0x11,0x11,0x11,0x11,0x11,0x0e}}, // U
        {{0x11,0x11,0x11,0x11,0x11,0x0a,0x04}}, // V
        {{0x11,0x11,0x11,0x15,0x15,0x15,0x0a}}, // W
        {{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}}, // X
        {{0x11,0x11,0x0a,0x04,0x04,0x04,0x04}}, // Y
        {{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}}, // Z
        {{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}}, // 0
        {{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}}, // 1
        {{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}}, // 2
        {{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e}}, // 3
        {{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}}, // 4
        {{0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e}}, // 5
        {{0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e}}, // 6
        {{0x1f,0x01,0x02,0x04,0x08,0x08,0x08}}, // 7
        {{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}}, // 8
        {{0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e}}, // 9
        {{0x00,0x04,0x04,0x1f,0x04,0x04,0x00}}, // -
        {{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // space
        {{0x00,0x00,0x00,0x00,0x00,0x00,0x04}}, // .
        {{0x0c,0x12,0x04,0x08,0x10,0x09,0x06}}, // %
    }};
    const auto upper = static_cast<char>(
        character >= 'a' && character <= 'z' ? character - ('a' - 'A') : character);
    const auto position = alphabet.find(upper);
    return position == std::string::npos ? font[37] : font[position];
}

void draw_text(ByteArray& image, unsigned int width, unsigned int height,
               int x, int y, std::string_view text, std::uint32_t color,
               std::uint32_t background, unsigned int scale) {
    const int glyph_width = static_cast<int>(6U * scale);
    const int glyph_height = static_cast<int>(8U * scale);
    const int start_x = x;
    for (const char character : text) {
        if (character == '\n') {
            y += glyph_height;
            x = start_x;
            continue;
        }
        const auto bitmap = glyph(character);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                const std::uint32_t pixel_color =
                    (bitmap[static_cast<std::size_t>(row)] &
                     (1U << (4U - static_cast<unsigned int>(column))))
                        ? color
                        : background;
                for (unsigned int dy = 0U; dy < scale; ++dy) {
                    for (unsigned int dx = 0U; dx < scale; ++dx) {
                        put_pixel(
                            image, width, height,
                            x + column * static_cast<int>(scale) +
                                static_cast<int>(dx),
                            y + row * static_cast<int>(scale) +
                                static_cast<int>(dy),
                            pixel_color);
                    }
                }
            }
        }
        x += glyph_width;
        if (x >= static_cast<int>(width)) {
            break;
        }
    }
}

std::unique_ptr<YoloProcessor> make_yolo(std::string_view settings_json) {
    const auto settings = plugin_json::parse_object(settings_json, kPluginName);
    const auto width = optional_unsigned(settings, "input_width", 640U, true);
    const auto height = optional_unsigned(settings, "input_height", 640U, true);
    const auto confidence = optional_float(settings, "confidence_threshold", 0.25F);
    const auto nms = optional_float(settings, "nms_threshold", 0.45F);
    if (!(confidence > 0.0F && confidence < 1.0F)) {
        plugin_json::fail(kPluginName, "confidence_threshold",
                          "must be between 0 and 1");
    }
    if (!(nms >= 0.0F && nms <= 1.0F)) {
        plugin_json::fail(kPluginName, "nms_threshold", "must be between 0 and 1");
    }
    const auto box_thickness =
        optional_unsigned(settings, "box_thickness", 4U, true);
    const auto label_scale =
        optional_unsigned(settings, "label_scale", 2U, true);
    if (box_thickness > 32U) {
        plugin_json::fail(kPluginName, "box_thickness", "must be between 1 and 32");
    }
    if (label_scale > 8U) {
        plugin_json::fail(kPluginName, "label_scale", "must be between 1 and 8");
    }
    return std::make_unique<YoloProcessor>(
        optional_string(settings, "model", "./model/yolov5s_rk3566.rknn"),
        optional_string(settings, "input_point", "image"),
        optional_string(settings, "output_point", "image"),
        optional_string(settings, "camera_device_id", "camera-1"),
        optional_string(settings, "camera_command", "capture"),
        optional_string(settings, "control_request_prefix", "yolo-capture"),
        width, height, confidence, nms,
        optional_unsigned(settings, "max_detections", 64U, true),
        optional_unsigned(settings, "control_timeout_ms", 5000U, true),
        optional_bool(settings, "draw_confidence", true), box_thickness,
        label_scale, parse_color(settings, "box_color", 0x00ff00U),
        parse_color(settings, "text_color", 0xffffffU));
}

}  // namespace

YoloProcessor::YoloProcessor(
    std::string model_path,
    std::string input_point,
    std::string output_point,
    std::string camera_device_id,
    std::string camera_command,
    std::string control_request_prefix,
    unsigned int input_width,
    unsigned int input_height,
    float confidence_threshold,
    float nms_threshold,
    unsigned int max_detections,
    unsigned int control_timeout_ms,
    bool draw_confidence,
    unsigned int box_thickness,
    unsigned int label_scale,
    std::uint32_t box_color,
    std::uint32_t text_color)
    : model_path_(std::move(model_path)),
      input_point_(std::move(input_point)),
      output_point_(std::move(output_point)),
      camera_device_id_(std::move(camera_device_id)),
      camera_command_(std::move(camera_command)),
      control_request_prefix_(std::move(control_request_prefix)),
      input_width_(input_width),
      input_height_(input_height),
      confidence_threshold_(confidence_threshold),
      nms_threshold_(nms_threshold),
      max_detections_(max_detections),
      control_timeout_ms_(control_timeout_ms),
      draw_confidence_(draw_confidence),
      box_thickness_(box_thickness),
      label_scale_(label_scale),
      box_color_(box_color),
      text_color_(text_color) {
    if (model_path_.empty() || input_point_.empty() || output_point_.empty() ||
        camera_device_id_.empty() || camera_command_.empty() ||
        control_request_prefix_.empty() || input_width_ == 0U ||
        input_height_ == 0U || max_detections_ == 0U ||
        control_timeout_ms_ == 0U || box_thickness_ == 0U ||
        label_scale_ == 0U) {
        throw std::invalid_argument("invalid YOLO processor settings");
    }
    initialize_model();
}

YoloProcessor::~YoloProcessor() {
    if (initialized_) {
        (void)rknn_destroy(context_);
        context_ = 0;
        initialized_ = false;
    }
}

void YoloProcessor::initialize_model() {
    std::ifstream model_file(model_path_, std::ios::binary | std::ios::ate);
    if (!model_file) {
        throw std::runtime_error("cannot open YOLO model: " + model_path_);
    }
    const auto model_size = model_file.tellg();
    if (model_size <= 0) {
        throw std::runtime_error("YOLO model is empty: " + model_path_);
    }
    model_data_.resize(static_cast<std::size_t>(model_size));
    model_file.seekg(0, std::ios::beg);
    model_file.read(
        reinterpret_cast<char*>(model_data_.data()),
        static_cast<std::streamsize>(model_size));
    if (!model_file) {
        throw std::runtime_error("cannot read YOLO model: " + model_path_);
    }

    const int result = rknn_init(
        &context_, model_data_.data(),
        static_cast<std::uint32_t>(model_data_.size()), 0U, nullptr);
    if (result < 0) {
        throw std::runtime_error("rknn_init failed: " + std::to_string(result));
    }

    rknn_input_output_num io_num{};
    if (rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io_num,
                  sizeof(io_num)) < 0 || io_num.n_input != 1U ||
        io_num.n_output == 0U) {
        (void)rknn_destroy(context_);
        context_ = 0;
        throw std::runtime_error(
            "YOLO model must have one input and at least one output");
    }

    input_attr_.index = 0U;
    if (rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_,
                  sizeof(input_attr_)) < 0) {
        (void)rknn_destroy(context_);
        context_ = 0;
        throw std::runtime_error("cannot query YOLO input tensor");
    }
    const auto input_elements = tensor_elements(input_attr_);
    const auto expected_elements = static_cast<std::uint64_t>(input_width_) *
                                   input_height_ * 3U;
    if (input_elements != expected_elements || input_attr_.n_dims != 4U) {
        (void)rknn_destroy(context_);
        context_ = 0;
        throw std::runtime_error(
            "YOLO input shape " + tensor_shape(input_attr_) +
            " is incompatible with configured RGB image " +
            std::to_string(input_width_) + "x" +
            std::to_string(input_height_));
    }

    outputs_.clear();
    outputs_.reserve(io_num.n_output);
    for (std::uint32_t index = 0U; index < io_num.n_output; ++index) {
        OutputTensor output;
        output.attr.index = index;
        if (rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &output.attr,
                      sizeof(output.attr)) < 0) {
            (void)rknn_destroy(context_);
            context_ = 0;
            throw std::runtime_error("cannot query YOLO output tensor");
        }

        // The supplied yolov5s_rk3566.rknn is exported with a single decoded
        // output [1, 25200, 85].  Also accept the three raw detection heads
        // emitted by the Rockchip sample model.
        if (output.attr.n_dims >= 3U) {
            const auto second_last = output.attr.n_dims - 2U;
            const auto last = output.attr.n_dims - 1U;
            const auto dim_a = output.attr.dims[second_last];
            const auto dim_b = output.attr.dims[last];
            if ((dim_a == 25200U && dim_b >= kAttributesPerAnchor) ||
                (dim_b == 25200U && dim_a >= kAttributesPerAnchor)) {
                output.kind = OutputKind::FlatDecoded;
                output.flat_attributes_first = dim_b == 25200U;
                output.flat_rows = output.flat_attributes_first ? dim_b : dim_a;
                output.flat_attributes = output.flat_attributes_first ? dim_a : dim_b;
                outputs_.push_back(output);
                continue;
            }
        }

        if (output.attr.n_dims < 3U || output.attr.n_dims > 4U) {
            continue;
        }
        if (output.attr.n_dims == 4U) {
            output.grid_height = output.attr.dims[2];
            output.grid_width = output.attr.dims[3];
            output.channels = output.attr.dims[1];
            if (output.attr.fmt == RKNN_TENSOR_NHWC) {
                output.grid_height = output.attr.dims[1];
                output.grid_width = output.attr.dims[2];
                output.channels = output.attr.dims[3];
            }
        } else if (output.attr.fmt == RKNN_TENSOR_NHWC) {
            output.grid_height = output.attr.dims[0];
            output.grid_width = output.attr.dims[1];
            output.channels = output.attr.dims[2];
        } else {
            output.channels = output.attr.dims[0];
            output.grid_height = output.attr.dims[1];
            output.grid_width = output.attr.dims[2];
        }
        if (output.channels != kAnchorCount * kAttributesPerAnchor ||
            output.grid_width == 0U || output.grid_height == 0U) {
            continue;
        }
        output.stride = input_height_ / output.grid_height;
        if (output.stride == 0U ||
            input_width_ / output.grid_width != output.stride) {
            continue;
        }
        output.kind = OutputKind::DetectionHead;
        outputs_.push_back(output);
    }

    const bool has_flat_output =
        outputs_.size() == 1U &&
        outputs_.front().kind == OutputKind::FlatDecoded;
    if (!has_flat_output) {
        outputs_.erase(
            std::remove_if(outputs_.begin(), outputs_.end(),
                           [](const OutputTensor& output) {
                               return output.kind != OutputKind::DetectionHead;
                           }),
            outputs_.end());
    }
    if ((!has_flat_output && outputs_.size() < 3U) || outputs_.empty()) {
        (void)rknn_destroy(context_);
        context_ = 0;
        throw std::runtime_error(
            "YOLO model outputs are neither one [1,25200,85] decoded output "
            "nor three 255-channel detection heads");
    }
    if (!has_flat_output) {
        outputs_.resize(3U);
        std::sort(outputs_.begin(), outputs_.end(),
                  [](const OutputTensor& left, const OutputTensor& right) {
                      return left.grid_width > right.grid_width;
                  });
    }

    initialized_ = true;
    std::cerr << "[yolo] model=" << model_path_
              << " input=" << tensor_shape(input_attr_)
              << " output_count=" << outputs_.size();
    for (const auto& output : outputs_) {
        std::cerr << " " << tensor_shape(output.attr) << ":"
                  << (output.kind == OutputKind::FlatDecoded ? "decoded" : "head");
    }
    std::cerr << '\n';
}

float YoloProcessor::output_value(
    const OutputTensor& output,
    const float* data,
    unsigned int channel,
    unsigned int row,
    unsigned int column) const {
    if (output.attr.n_dims == 4U && output.attr.fmt == RKNN_TENSOR_NHWC) {
        const auto index = ((static_cast<std::size_t>(row) * output.grid_width +
                             column) * output.channels) + channel;
        return data[index];
    }
    const auto index = (static_cast<std::size_t>(channel) * output.grid_height +
                        row) * output.grid_width + column;
    return data[index];
}

float YoloProcessor::flat_output_value(
    const OutputTensor& output,
    const float* data,
    unsigned int row,
    unsigned int attribute) const {
    if (output.flat_attributes_first) {
        return data[static_cast<std::size_t>(attribute) * output.flat_rows + row];
    }
    return data[static_cast<std::size_t>(row) * output.flat_attributes + attribute];
}

std::vector<YoloProcessor::Detection> YoloProcessor::infer(
    const ByteArray& image) {
    const auto expected_size = static_cast<std::size_t>(input_width_) *
                               input_height_ * 3U;
    if (image.size() != expected_size) {
        throw std::runtime_error(
            "YOLO expects RGB24 " + std::to_string(input_width_) + "x" +
            std::to_string(input_height_) + " (" +
            std::to_string(expected_size) + " bytes), got " +
            std::to_string(image.size()));
    }

    rknn_input input{};
    input.index = 0U;
    input.buf = const_cast<std::uint8_t*>(image.data());
    input.size = static_cast<std::uint32_t>(image.size());
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.pass_through = 0U;
    if (rknn_inputs_set(context_, 1U, &input) < 0 ||
        rknn_run(context_, nullptr) < 0) {
        throw std::runtime_error("YOLO NPU inference failed");
    }

    std::vector<rknn_output> raw_outputs(outputs_.size());
    for (std::size_t index = 0U; index < raw_outputs.size(); ++index) {
        raw_outputs[index].index = outputs_[index].attr.index;
        raw_outputs[index].want_float = 1U;
    }
    if (rknn_outputs_get(context_, static_cast<std::uint32_t>(raw_outputs.size()),
                        raw_outputs.data(), nullptr) < 0) {
        throw std::runtime_error("cannot read YOLO NPU outputs");
    }

    std::vector<Detection> candidates;
    try {
        const auto to_probability = [](float value) {
            return value >= 0.0F && value <= 1.0F ? value : sigmoid(value);
        };
        const auto clamp_detection = [this](Detection detection) {
            detection.left = std::clamp(
                detection.left, 0, static_cast<int>(input_width_) - 1);
            detection.top = std::clamp(
                detection.top, 0, static_cast<int>(input_height_) - 1);
            detection.right = std::clamp(
                detection.right, 0, static_cast<int>(input_width_) - 1);
            detection.bottom = std::clamp(
                detection.bottom, 0, static_cast<int>(input_height_) - 1);
            if (detection.right <= detection.left ||
                detection.bottom <= detection.top) {
                return std::optional<Detection>{};
            }
            return std::optional<Detection>{detection};
        };

        if (outputs_.size() == 1U &&
            outputs_.front().kind == OutputKind::FlatDecoded) {
            const auto& output = outputs_.front();
            const auto* data = static_cast<const float*>(raw_outputs.front().buf);
            if (data == nullptr) {
                throw std::runtime_error("YOLO output buffer is null");
            }
            for (unsigned int row = 0U; row < output.flat_rows; ++row) {
                const float object_probability = to_probability(
                    flat_output_value(output, data, row, 4U));
                if (object_probability < confidence_threshold_) {
                    continue;
                }

                int class_id = 0;
                float best_class = 0.0F;
                for (unsigned int cls = 0U; cls < kClassCount; ++cls) {
                    const float probability = to_probability(
                        flat_output_value(output, data, row, 5U + cls));
                    if (probability > best_class) {
                        best_class = probability;
                        class_id = static_cast<int>(cls);
                    }
                }
                const float confidence = object_probability * best_class;
                if (confidence < confidence_threshold_) {
                    continue;
                }

                // The ONNX used to generate this RKNN model contains the
                // YOLOv5 Detect export node, so x/y/w/h are already decoded
                // into input-pixel coordinates in the flat output.
                const float center_x = flat_output_value(output, data, row, 0U);
                const float center_y = flat_output_value(output, data, row, 1U);
                const float box_width = flat_output_value(output, data, row, 2U);
                const float box_height = flat_output_value(output, data, row, 3U);
                if (!std::isfinite(center_x) || !std::isfinite(center_y) ||
                    !std::isfinite(box_width) || !std::isfinite(box_height) ||
                    box_width <= 0.0F || box_height <= 0.0F) {
                    continue;
                }
                const auto detection = clamp_detection(Detection{
                    .left = static_cast<int>(std::floor(
                        center_x - box_width / 2.0F)),
                    .top = static_cast<int>(std::floor(
                        center_y - box_height / 2.0F)),
                    .right = static_cast<int>(std::ceil(
                        center_x + box_width / 2.0F)),
                    .bottom = static_cast<int>(std::ceil(
                        center_y + box_height / 2.0F)),
                    .class_id = class_id,
                    .confidence = confidence,
                });
                if (detection.has_value()) {
                    candidates.push_back(*detection);
                }
            }
        } else {
            for (std::size_t head_index = 0U;
                 head_index < outputs_.size(); ++head_index) {
                const auto& head = outputs_[head_index];
                const auto* data = static_cast<const float*>(
                    raw_outputs[head_index].buf);
                if (data == nullptr) {
                    throw std::runtime_error("YOLO output buffer is null");
                }
                const auto& anchors = kAnchors[head_index];
                for (unsigned int anchor = 0U; anchor < kAnchorCount; ++anchor) {
                    const auto base = anchor * kAttributesPerAnchor;
                    for (unsigned int row = 0U; row < head.grid_height; ++row) {
                        for (unsigned int column = 0U;
                             column < head.grid_width; ++column) {
                            const float object_probability = to_probability(
                                output_value(head, data, base + 4U, row, column));
                            if (object_probability < confidence_threshold_) {
                                continue;
                            }
                            int class_id = 0;
                            float best_class = 0.0F;
                            for (unsigned int cls = 0U; cls < kClassCount; ++cls) {
                                const float probability = to_probability(
                                    output_value(head, data, base + 5U + cls,
                                                 row, column));
                                if (probability > best_class) {
                                    best_class = probability;
                                    class_id = static_cast<int>(cls);
                                }
                            }
                            const float confidence = object_probability * best_class;
                            if (confidence < confidence_threshold_) {
                                continue;
                            }

                            const float x = output_value(
                                head, data, base, row, column);
                            const float y = output_value(
                                head, data, base + 1U, row, column);
                            const float w = output_value(
                                head, data, base + 2U, row, column);
                            const float h = output_value(
                                head, data, base + 3U, row, column);
                            const float center_x =
                                (x * 2.0F - 0.5F + column) * head.stride;
                            const float center_y =
                                (y * 2.0F - 0.5F + row) * head.stride;
                            const float box_width = std::pow(w * 2.0F, 2.0F) *
                                static_cast<float>(anchors[anchor * 2U]);
                            const float box_height = std::pow(h * 2.0F, 2.0F) *
                                static_cast<float>(anchors[anchor * 2U + 1U]);
                            const auto detection = clamp_detection(Detection{
                                .left = static_cast<int>(std::floor(
                                    center_x - box_width / 2.0F)),
                                .top = static_cast<int>(std::floor(
                                    center_y - box_height / 2.0F)),
                                .right = static_cast<int>(std::ceil(
                                    center_x + box_width / 2.0F)),
                                .bottom = static_cast<int>(std::ceil(
                                    center_y + box_height / 2.0F)),
                                .class_id = class_id,
                                .confidence = confidence,
                            });
                            if (detection.has_value()) {
                                candidates.push_back(*detection);
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
        (void)rknn_outputs_release(
            context_, static_cast<std::uint32_t>(raw_outputs.size()),
            raw_outputs.data());
        throw;
    }
    (void)rknn_outputs_release(
        context_, static_cast<std::uint32_t>(raw_outputs.size()),
        raw_outputs.data());

    std::sort(candidates.begin(), candidates.end(),
              [](const Detection& left, const Detection& right) {
                  return left.confidence > right.confidence;
              });
    std::vector<bool> suppressed(candidates.size(), false);
    std::vector<Detection> result;
    result.reserve(std::min<std::size_t>(max_detections_, candidates.size()));
    for (std::size_t index = 0U; index < candidates.size(); ++index) {
        if (suppressed[index]) {
            continue;
        }
        result.push_back(candidates[index]);
        if (result.size() >= max_detections_) {
            break;
        }
        for (std::size_t other = index + 1U; other < candidates.size(); ++other) {
            if (!suppressed[other] &&
                candidates[other].class_id == candidates[index].class_id &&
                overlap(candidates[index], candidates[other]) > nms_threshold_) {
                suppressed[other] = true;
            }
        }
    }
    return result;
}

void YoloProcessor::annotate(
    ByteArray& image,
    const std::vector<Detection>& detections) const {
    for (const auto& detection : detections) {
        draw_rect(image, input_width_, input_height_, detection, box_color_,
                  box_thickness_);
        std::string label = detection.class_id >= 0 &&
                                    static_cast<std::size_t>(detection.class_id) <
                                        kCocoLabels.size()
                                ? kCocoLabels[static_cast<std::size_t>(detection.class_id)]
                                : "unknown";
        if (draw_confidence_) {
            label += " " + std::to_string(static_cast<int>(
                std::lround(detection.confidence * 100.0F))) + "%";
        }
        const int padding = static_cast<int>(label_scale_);
        const int label_height = static_cast<int>(7U * label_scale_) +
                                 padding * 2;
        const int label_width = static_cast<int>(label.size() * 6U * label_scale_) +
                                padding * 2;
        const int label_x = std::max(0, detection.left);
        const int label_y = std::max(0, detection.top - label_height);
        for (int y = label_y; y < label_y + label_height; ++y) {
            for (int x = label_x; x < label_x + label_width; ++x) {
                put_pixel(image, input_width_, input_height_, x, y, box_color_);
            }
        }
        draw_text(image, input_width_, input_height_, label_x + padding,
                  label_y + padding, label, text_color_, box_color_,
                  label_scale_);
    }
}

void YoloProcessor::request_next_frame(
    const Event& event,
    ProcessingContext& context) const {
    DeviceControlRequest request{
        .request_id = control_request_prefix_ + "-" + event.event_id,
        .device_id = camera_device_id_,
        .command = camera_command_,
        .arguments = {},
        .deadline = ControlClock::now() +
                    std::chrono::milliseconds{control_timeout_ms_},
    };
    const auto result = context.submit_control(std::move(request));
    if (result != ControlSubmitResult::Accepted) {
        std::cerr << "[yolo] failed to request next camera frame, result="
                  << static_cast<int>(result) << " event=" << event.event_id
                  << "\n";
    }
}

void YoloProcessor::process(Event& event, ProcessingContext& context) {
    if (event.device_id != camera_device_id_) {
        return;
    }
    auto* image_reading = find_reading(event, input_point_);
    if (image_reading == nullptr) {
        for (auto& reading : event.readings) {
            if (std::holds_alternative<ByteArray>(reading.value)) {
                image_reading = &reading;
                break;
            }
        }
    }
    if (image_reading == nullptr || image_reading->quality != Quality::Good ||
        !std::holds_alternative<ByteArray>(image_reading->value)) {
        std::cerr << "[yolo] event has no valid RGB image: " << event.event_id
                  << "\n";
        request_next_frame(event, context);
        return;
    }

    try {
        auto& image = std::get<ByteArray>(image_reading->value);
        const auto detections = infer(image);
        annotate(image, detections);
        if (output_point_ != input_point_) {
            auto* output = find_reading(event, output_point_);
            if (output != nullptr &&
                std::holds_alternative<ByteArray>(output->value)) {
                output->value = image;
                output->quality = Quality::Good;
            }
        }
        event.model_version = "yolov5s-rk3566-rknn";
        std::cerr << "[yolo] event=" << event.event_id
                  << " detections=" << detections.size() << "\n";
    } catch (const std::exception& error) {
        // A bad frame or a transient RKNN error must not stop the controlled
        // camera -> processor -> screen loop. Request another frame even
        // when this event could not be annotated.
        std::cerr << "[yolo] processing failed for event=" << event.event_id
                  << ": " << error.what() << "\n";
    } catch (...) {
        std::cerr << "[yolo] processing failed for event=" << event.event_id
                  << ": unknown error\n";
    }
    request_next_frame(event, context);
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_yolo(settings_json == nullptr ? std::string_view{"{}"}
                                                 : std::string_view{settings_json})
            .release();
    } catch (const std::exception& error) {
        std::cerr << "[yolo] create failed: " << error.what() << "\n";
        return nullptr;
    } catch (...) {
        std::cerr << "[yolo] create failed with unknown error\n";
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<YoloProcessor*>(plugin);
}

}  // namespace gateway
