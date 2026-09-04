#include "yolo.hpp"

#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"
#include "onnxruntime_cxx_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cctype>
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

constexpr std::string_view kPluginName{"yolov5 processor"};
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

YoloProcessor::InferenceBackend parse_backend(
    const plugin_json::Json& settings) {
    auto backend = optional_string(settings, "inference_backend", "rknn");
    std::transform(backend.begin(), backend.end(), backend.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (backend == "rknn" || backend == "npu") {
        return YoloProcessor::InferenceBackend::Rknn;
    }
    if (backend == "onnxruntime" || backend == "onnx" || backend == "cpu") {
        return YoloProcessor::InferenceBackend::OnnxRuntime;
    }
    plugin_json::fail(
        kPluginName, "inference_backend",
        "must be one of: rknn, npu, onnxruntime, onnx, cpu");
}

std::string select_model_path(
    const plugin_json::Json& settings,
    YoloProcessor::InferenceBackend backend) {
    // "model" remains an explicit override.  With rknn_model and onnx_model
    // both configured, switching inference_backend is enough to switch the
    // engine and its corresponding model.
    if (optional_member(settings, "model") != nullptr) {
        return optional_string(settings, "model", "");
    }
    if (backend == YoloProcessor::InferenceBackend::Rknn) {
        return optional_string(
            settings, "rknn_model", "./model/yolov5s_rk3566.rknn");
    }
    return optional_string(
        settings, "onnx_model", "./model/yolov5n.onnx");
}

std::unique_ptr<YoloProcessor> make_yolo(std::string_view settings_json) {
    const auto settings = plugin_json::parse_object(settings_json, kPluginName);
    const auto backend = parse_backend(settings);
    const auto width = optional_unsigned(settings, "input_width", 640U, true);
    const auto height = optional_unsigned(settings, "input_height", 640U, true);
    const auto confidence = optional_float(settings, "confidence_threshold", 0.25F);
    const auto nms = optional_float(settings, "nms_threshold", 0.45F);
    const auto onnx_input_scale =
        optional_float(settings, "onnx_input_scale", 1.0F / 255.0F);
    if (!(confidence > 0.0F && confidence < 1.0F)) {
        plugin_json::fail(kPluginName, "confidence_threshold",
                          "must be between 0 and 1");
    }
    if (!(nms >= 0.0F && nms <= 1.0F)) {
        plugin_json::fail(kPluginName, "nms_threshold", "must be between 0 and 1");
    }
    if (!(onnx_input_scale > 0.0F) || !std::isfinite(onnx_input_scale)) {
        plugin_json::fail(kPluginName, "onnx_input_scale",
                          "must be a finite positive number");
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
        backend, select_model_path(settings, backend),
        optional_string(settings, "input_point", "image"),
        optional_string(settings, "output_point", "image"),
        optional_string(settings, "camera_device_id", "camera-1"),
        optional_string(settings, "camera_command", "capture"),
        optional_string(settings, "control_request_prefix", "yolo-capture"),
        width, height, confidence, nms,
        optional_unsigned(settings, "max_detections", 64U, true),
        optional_unsigned(settings, "control_timeout_ms", 5000U, true),
        optional_unsigned(settings, "onnx_intra_op_threads", 1U, true),
        optional_unsigned(settings, "onnx_inter_op_threads", 1U, true),
        onnx_input_scale,
        optional_bool(settings, "draw_confidence", true), box_thickness,
        label_scale, parse_color(settings, "box_color", 0x00ff00U),
        parse_color(settings, "text_color", 0xffffffU));
}

}  // namespace

struct YoloProcessor::OnnxState {
    enum class InputType {
        Float32,
        Float16,
        UInt8,
    };

    OnnxState(unsigned int intra_op_threads, unsigned int inter_op_threads)
        : environment(ORT_LOGGING_LEVEL_WARNING, "miniGateway-yolo"),
          session(nullptr) {
        session_options.SetIntraOpNumThreads(
            static_cast<int>(intra_op_threads));
        session_options.SetInterOpNumThreads(
            static_cast<int>(inter_op_threads));
        session_options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        session_options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
    }

    Ort::Env environment;
    Ort::SessionOptions session_options;
    Ort::Session session;
    std::string input_name;
    std::vector<std::string> output_names;
    std::vector<float> float_input;
    std::vector<Ort::Float16_t> float16_input;
    std::vector<std::uint8_t> uint8_input;
    std::vector<float> float_output;
    bool input_nchw{true};
    InputType input_type{InputType::Float32};
};

YoloProcessor::YoloProcessor(
    InferenceBackend backend,
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
    unsigned int onnx_intra_op_threads,
    unsigned int onnx_inter_op_threads,
    float onnx_input_scale,
    bool draw_confidence,
    unsigned int box_thickness,
    unsigned int label_scale,
    std::uint32_t box_color,
    std::uint32_t text_color)
    : backend_(backend),
      model_path_(std::move(model_path)),
      model_version_(backend == InferenceBackend::Rknn
                         ? "yolov5-rknn-npu"
                         : "yolov5-onnxruntime-cpu"),
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
      onnx_intra_op_threads_(onnx_intra_op_threads),
      onnx_inter_op_threads_(onnx_inter_op_threads),
      onnx_input_scale_(onnx_input_scale),
      draw_confidence_(draw_confidence),
      box_thickness_(box_thickness),
      label_scale_(label_scale),
      box_color_(box_color),
      text_color_(text_color) {
    if (model_path_.empty() || input_point_.empty() || output_point_.empty() ||
        camera_device_id_.empty() || camera_command_.empty() ||
        control_request_prefix_.empty() || input_width_ == 0U ||
        input_height_ == 0U || max_detections_ == 0U ||
        control_timeout_ms_ == 0U || onnx_intra_op_threads_ == 0U ||
        onnx_inter_op_threads_ == 0U || !(onnx_input_scale_ > 0.0F) ||
        box_thickness_ == 0U || label_scale_ == 0U) {
        throw std::invalid_argument("invalid YOLO processor settings");
    }
    initialize_model();
}

YoloProcessor::~YoloProcessor() {
    onnx_state_.reset();
    if (context_ != 0) {
        (void)rknn_destroy(context_);
        context_ = 0;
    }
    initialized_ = false;
}

void YoloProcessor::initialize_model() {
    if (backend_ == InferenceBackend::Rknn) {
        initialize_rknn_model();
    } else {
        initialize_onnx_model();
    }
    initialized_ = true;
}

void YoloProcessor::initialize_rknn_model() {
    std::ifstream model_file(model_path_, std::ios::binary | std::ios::ate);
    if (!model_file) {
        throw std::runtime_error("cannot open YOLO RKNN model: " + model_path_);
    }
    const auto model_size = model_file.tellg();
    if (model_size <= 0) {
        throw std::runtime_error("YOLO RKNN model is empty: " + model_path_);
    }
    model_data_.resize(static_cast<std::size_t>(model_size));
    model_file.seekg(0, std::ios::beg);
    model_file.read(
        reinterpret_cast<char*>(model_data_.data()),
        static_cast<std::streamsize>(model_size));
    if (!model_file) {
        throw std::runtime_error("cannot read YOLO RKNN model: " + model_path_);
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
            "YOLO RKNN model must have one input and at least one output");
    }

    input_attr_.index = 0U;
    if (rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_,
                  sizeof(input_attr_)) < 0) {
        (void)rknn_destroy(context_);
        context_ = 0;
        throw std::runtime_error("cannot query YOLO RKNN input tensor");
    }
    const auto input_elements = tensor_elements(input_attr_);
    const auto expected_elements = static_cast<std::uint64_t>(input_width_) *
                                   input_height_ * 3U;
    if (input_elements != expected_elements || input_attr_.n_dims != 4U) {
        (void)rknn_destroy(context_);
        context_ = 0;
        throw std::runtime_error(
            "YOLO RKNN input shape " + tensor_shape(input_attr_) +
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
            throw std::runtime_error("cannot query YOLO RKNN output tensor");
        }

        if (output.attr.n_dims >= 3U) {
            const auto second_last = output.attr.n_dims - 2U;
            const auto last = output.attr.n_dims - 1U;
            const auto dim_a = output.attr.dims[second_last];
            const auto dim_b = output.attr.dims[last];
            if ((dim_a > 0U && dim_b >= kAttributesPerAnchor && dim_b <= 512U) ||
                (dim_b > 0U && dim_a >= kAttributesPerAnchor && dim_a <= 512U)) {
                output.kind = OutputKind::FlatDecoded;
                output.flat_attributes_first = dim_a <= 512U && dim_b > 512U;
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
            "YOLO RKNN outputs are neither one decoded [1,N,85] tensor "
            "nor three 255-channel detection heads");
    }
    if (!has_flat_output) {
        outputs_.resize(3U);
        std::sort(outputs_.begin(), outputs_.end(),
                  [](const OutputTensor& left, const OutputTensor& right) {
                      return left.grid_width > right.grid_width;
                  });
    }

    std::cerr << "[yolo] backend=rknn model=" << model_path_
              << " input=" << tensor_shape(input_attr_)
              << " output_count=" << outputs_.size();
    for (const auto& output : outputs_) {
        std::cerr << " " << tensor_shape(output.attr) << ":"
                  << (output.kind == OutputKind::FlatDecoded ? "decoded" : "head");
    }
    std::cerr << '\n';
}

void YoloProcessor::initialize_onnx_model() {
    onnx_state_ = std::make_unique<OnnxState>(
        onnx_intra_op_threads_, onnx_inter_op_threads_);
    try {
        onnx_state_->session = Ort::Session(
            onnx_state_->environment, model_path_.c_str(),
            onnx_state_->session_options);

        const std::size_t input_count = onnx_state_->session.GetInputCount();
        const std::size_t output_count = onnx_state_->session.GetOutputCount();
        if (input_count != 1U || output_count == 0U) {
            throw std::runtime_error(
                "YOLO ONNX model must have one input and at least one output");
        }

        Ort::AllocatorWithDefaultOptions allocator;
        auto input_name =
            onnx_state_->session.GetInputNameAllocated(0U, allocator);
        onnx_state_->input_name = input_name.get();

        // Keep the owning Ort::TypeInfo alive while reading the unowned
        // TensorTypeAndShapeInfo view returned by GetTensorTypeAndShapeInfo().
        // Chaining these calls through the temporary returned by
        // GetInputTypeInfo() leaves a dangling view on some AArch64 builds
        // of ONNX Runtime and can make a valid [1,3,640,640] model appear
        // to have an invalid rank/batch shape.
        const auto input_type_info = onnx_state_->session.GetInputTypeInfo(0U);
        const auto input_info = input_type_info.GetTensorTypeAndShapeInfo();
        const auto input_shape = input_info.GetShape();
        if (input_shape.size() != 4U ||
            (input_shape[0] > 0 && input_shape[0] != 1)) {
            throw std::runtime_error(
                "YOLO ONNX input must be a rank-4 tensor with batch size 1");
        }
        if (input_shape[1] == 3 ||
            (input_shape[1] < 0 && input_shape[3] != 3)) {
            onnx_state_->input_nchw = true;
            if ((input_shape[2] > 0 &&
                 input_shape[2] != static_cast<std::int64_t>(input_height_)) ||
                (input_shape[3] > 0 &&
                 input_shape[3] != static_cast<std::int64_t>(input_width_))) {
                throw std::runtime_error(
                    "YOLO ONNX NCHW input size does not match input_width/input_height");
            }
        } else if (input_shape[3] == 3 || input_shape[3] < 0) {
            onnx_state_->input_nchw = false;
            if ((input_shape[1] > 0 &&
                 input_shape[1] != static_cast<std::int64_t>(input_height_)) ||
                (input_shape[2] > 0 &&
                 input_shape[2] != static_cast<std::int64_t>(input_width_))) {
                throw std::runtime_error(
                    "YOLO ONNX NHWC input size does not match input_width/input_height");
            }
        } else {
            throw std::runtime_error(
                "YOLO ONNX input must have three RGB channels in NCHW or NHWC layout");
        }

        const auto input_type = input_info.GetElementType();
        if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            onnx_state_->input_type = OnnxState::InputType::Float32;
        } else if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            onnx_state_->input_type = OnnxState::InputType::Float16;
        } else if (input_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {
            onnx_state_->input_type = OnnxState::InputType::UInt8;
        } else {
            throw std::runtime_error(
                "YOLO ONNX input type must be float32, float16, or uint8");
        }

        onnx_state_->output_names.reserve(output_count);
        for (std::size_t index = 0U; index < output_count; ++index) {
            auto output_name =
                onnx_state_->session.GetOutputNameAllocated(index, allocator);
            onnx_state_->output_names.emplace_back(output_name.get());
        }

        const char* input_type_name = "uint8";
        if (onnx_state_->input_type == OnnxState::InputType::Float32) {
            input_type_name = "float32";
        } else if (onnx_state_->input_type == OnnxState::InputType::Float16) {
            input_type_name = "float16";
        }
        std::cerr << "[yolo] backend=onnxruntime model=" << model_path_
                  << " input=" << (onnx_state_->input_nchw ? "NCHW" : "NHWC")
                  << " type=" << input_type_name
                  << " size=" << input_width_ << "x" << input_height_
                  << " outputs=" << output_count
                  << " intra_threads=" << onnx_intra_op_threads_
                  << " inter_threads=" << onnx_inter_op_threads_ << '\n';
    } catch (...) {
        onnx_state_.reset();
        throw;
    }
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

std::vector<YoloProcessor::Detection> YoloProcessor::decode_flat_output(
    const float* data,
    std::size_t rows,
    std::size_t attributes,
    bool attributes_first) const {
    if (data == nullptr || rows == 0U || attributes < kAttributesPerAnchor) {
        throw std::runtime_error("YOLO decoded output has an invalid shape");
    }

    const auto value_at = [=](std::size_t row, std::size_t attribute) {
        return attributes_first ? data[attribute * rows + row]
                                : data[row * attributes + attribute];
    };
    const auto to_probability = [](float value) {
        return value >= 0.0F && value <= 1.0F ? value : sigmoid(value);
    };

    std::vector<Detection> candidates;
    candidates.reserve(std::min<std::size_t>(rows, 1024U));
    for (std::size_t row = 0U; row < rows; ++row) {
        const float object_probability = to_probability(value_at(row, 4U));
        if (!std::isfinite(object_probability) ||
            object_probability < confidence_threshold_) {
            continue;
        }

        int class_id = 0;
        float best_class = 0.0F;
        for (std::size_t cls = 0U; cls < kClassCount; ++cls) {
            const float probability = to_probability(value_at(row, 5U + cls));
            if (std::isfinite(probability) && probability > best_class) {
                best_class = probability;
                class_id = static_cast<int>(cls);
            }
        }
        const float confidence = object_probability * best_class;
        if (!std::isfinite(confidence) || confidence < confidence_threshold_) {
            continue;
        }

        const float center_x = value_at(row, 0U);
        const float center_y = value_at(row, 1U);
        const float box_width = value_at(row, 2U);
        const float box_height = value_at(row, 3U);
        if (!std::isfinite(center_x) || !std::isfinite(center_y) ||
            !std::isfinite(box_width) || !std::isfinite(box_height) ||
            box_width <= 0.0F || box_height <= 0.0F) {
            continue;
        }

        Detection detection{
            .left = static_cast<int>(std::floor(center_x - box_width / 2.0F)),
            .top = static_cast<int>(std::floor(center_y - box_height / 2.0F)),
            .right = static_cast<int>(std::ceil(center_x + box_width / 2.0F)),
            .bottom = static_cast<int>(std::ceil(center_y + box_height / 2.0F)),
            .class_id = class_id,
            .confidence = confidence,
        };
        detection.left = std::clamp(
            detection.left, 0, static_cast<int>(input_width_) - 1);
        detection.top = std::clamp(
            detection.top, 0, static_cast<int>(input_height_) - 1);
        detection.right = std::clamp(
            detection.right, 0, static_cast<int>(input_width_) - 1);
        detection.bottom = std::clamp(
            detection.bottom, 0, static_cast<int>(input_height_) - 1);
        if (detection.right > detection.left &&
            detection.bottom > detection.top) {
            candidates.push_back(detection);
        }
    }
    return candidates;
}

std::vector<YoloProcessor::Detection> YoloProcessor::apply_nms(
    std::vector<Detection> candidates) const {
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

std::vector<YoloProcessor::Detection> YoloProcessor::infer(
    const ByteArray& image) {
    if (backend_ == InferenceBackend::Rknn) {
        return infer_rknn(image);
    }
    return infer_onnx(image);
}

std::vector<YoloProcessor::Detection> YoloProcessor::infer_onnx(
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
    if (onnx_state_ == nullptr) {
        throw std::runtime_error("YOLO ONNX Runtime session is not initialized");
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(input_width_) * input_height_;
    const std::array<std::int64_t, 4> input_shape = onnx_state_->input_nchw
        ? std::array<std::int64_t, 4>{
              1, 3, static_cast<std::int64_t>(input_height_),
              static_cast<std::int64_t>(input_width_)}
        : std::array<std::int64_t, 4>{
              1, static_cast<std::int64_t>(input_height_),
              static_cast<std::int64_t>(input_width_), 3};

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor{nullptr};
    const auto source_value = [this, &image, pixel_count](
                                  std::size_t tensor_index) {
        if (!onnx_state_->input_nchw) {
            return static_cast<float>(image[tensor_index]) * onnx_input_scale_;
        }
        const std::size_t channel = tensor_index / pixel_count;
        const std::size_t pixel = tensor_index % pixel_count;
        return static_cast<float>(image[pixel * 3U + channel]) *
               onnx_input_scale_;
    };

    if (onnx_state_->input_type == OnnxState::InputType::Float32) {
        onnx_state_->float_input.resize(expected_size);
        for (std::size_t index = 0U; index < expected_size; ++index) {
            onnx_state_->float_input[index] = source_value(index);
        }
        input_tensor = Ort::Value::CreateTensor<float>(
            memory_info, onnx_state_->float_input.data(),
            onnx_state_->float_input.size(), input_shape.data(),
            input_shape.size());
    } else if (onnx_state_->input_type == OnnxState::InputType::Float16) {
        onnx_state_->float16_input.resize(expected_size);
        for (std::size_t index = 0U; index < expected_size; ++index) {
            onnx_state_->float16_input[index] = Ort::Float16_t(source_value(index));
        }
        input_tensor = Ort::Value::CreateTensor<Ort::Float16_t>(
            memory_info, onnx_state_->float16_input.data(),
            onnx_state_->float16_input.size(), input_shape.data(),
            input_shape.size());
    } else {
        onnx_state_->uint8_input.resize(expected_size);
        if (onnx_state_->input_nchw) {
            for (std::size_t tensor_index = 0U;
                 tensor_index < expected_size; ++tensor_index) {
                const std::size_t channel = tensor_index / pixel_count;
                const std::size_t pixel = tensor_index % pixel_count;
                onnx_state_->uint8_input[tensor_index] =
                    image[pixel * 3U + channel];
            }
        } else {
            std::copy(image.begin(), image.end(), onnx_state_->uint8_input.begin());
        }
        input_tensor = Ort::Value::CreateTensor<std::uint8_t>(
            memory_info, onnx_state_->uint8_input.data(),
            onnx_state_->uint8_input.size(), input_shape.data(),
            input_shape.size());
    }

    const char* input_name = onnx_state_->input_name.c_str();
    std::vector<const char*> output_names;
    output_names.reserve(onnx_state_->output_names.size());
    for (const auto& name : onnx_state_->output_names) {
        output_names.push_back(name.c_str());
    }

    auto output_tensors = onnx_state_->session.Run(
        Ort::RunOptions{nullptr}, &input_name, &input_tensor, 1U,
        output_names.data(), output_names.size());

    for (std::size_t index = 0U; index < output_tensors.size(); ++index) {
        auto& tensor = output_tensors[index];
        if (!tensor.IsTensor()) {
            continue;
        }
        const auto info = tensor.GetTensorTypeAndShapeInfo();
        const auto output_type = info.GetElementType();
        if (output_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            output_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            continue;
        }
        const auto shape = info.GetShape();
        if (shape.size() != 3U || shape[0] != 1 ||
            shape[1] <= 0 || shape[2] <= 0) {
            continue;
        }

        const auto dim_a = static_cast<std::size_t>(shape[1]);
        const auto dim_b = static_cast<std::size_t>(shape[2]);
        std::size_t rows = 0U;
        std::size_t attributes = 0U;
        bool attributes_first = false;
        if (dim_b >= kAttributesPerAnchor && dim_b <= 512U) {
            rows = dim_a;
            attributes = dim_b;
        } else if (dim_a >= kAttributesPerAnchor && dim_a <= 512U) {
            rows = dim_b;
            attributes = dim_a;
            attributes_first = true;
        } else {
            continue;
        }

        const float* data = nullptr;
        if (output_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            data = tensor.GetTensorData<float>();
        } else {
            const auto element_count = info.GetElementCount();
            const auto* float16_data = tensor.GetTensorData<Ort::Float16_t>();
            onnx_state_->float_output.resize(element_count);
            std::transform(
                float16_data, float16_data + element_count,
                onnx_state_->float_output.begin(),
                [](const Ort::Float16_t value) { return value.ToFloat(); });
            data = onnx_state_->float_output.data();
        }
        auto candidates = decode_flat_output(
            data, rows, attributes, attributes_first);
        return apply_nms(std::move(candidates));
    }

    throw std::runtime_error(
        "YOLO ONNX model has no float32/float16 decoded output shaped "
        "[1,N,85] or [1,85,N]");
}

std::vector<YoloProcessor::Detection> YoloProcessor::infer_rknn(
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
                                (x * 2.0F - 0.5F + static_cast<float>(column)) *
                                static_cast<float>(head.stride);
                            const float center_y =
                                (y * 2.0F - 0.5F + static_cast<float>(row)) *
                                static_cast<float>(head.stride);
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

    return apply_nms(std::move(candidates));
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
        event.model_version = model_version_;
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
