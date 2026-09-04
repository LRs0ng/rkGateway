#pragma once

#include "gateway/processing.hpp"
#include "rknn_api.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gateway {

class YoloProcessor final : public IDataProcessor {
public:
    enum class InferenceBackend {
        Rknn,
        OnnxRuntime,
    };

    YoloProcessor(
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
        std::uint32_t text_color);
    ~YoloProcessor() override;

    void process(Event& event, ProcessingContext& context) override;

    // Public so the small drawing/NMS helpers in the implementation can use
    // the same representation without making them class friends.
    struct Detection {
        int left{0};
        int top{0};
        int right{0};
        int bottom{0};
        int class_id{0};
        float confidence{0.0F};
    };

private:
    enum class OutputKind {
        FlatDecoded,
        DetectionHead,
    };

    struct OutputTensor {
        rknn_tensor_attr attr{};
        OutputKind kind{OutputKind::DetectionHead};
        unsigned int grid_width{0};
        unsigned int grid_height{0};
        unsigned int channels{0};
        unsigned int stride{0};
        unsigned int flat_rows{0};
        unsigned int flat_attributes{0};
        bool flat_attributes_first{false};
    };

    struct OnnxState;

    void initialize_model();
    void initialize_rknn_model();
    void initialize_onnx_model();
    [[nodiscard]] std::vector<Detection> infer(const ByteArray& image);
    [[nodiscard]] std::vector<Detection> infer_rknn(const ByteArray& image);
    [[nodiscard]] std::vector<Detection> infer_onnx(const ByteArray& image);
    [[nodiscard]] std::vector<Detection> decode_flat_output(
        const float* data,
        std::size_t rows,
        std::size_t attributes,
        bool attributes_first) const;
    [[nodiscard]] std::vector<Detection> apply_nms(
        std::vector<Detection> candidates) const;
    [[nodiscard]] float output_value(
        const OutputTensor& output,
        const float* data,
        unsigned int channel,
        unsigned int row,
        unsigned int column) const;
    [[nodiscard]] float flat_output_value(
        const OutputTensor& output,
        const float* data,
        unsigned int row,
        unsigned int attribute) const;
    void annotate(ByteArray& image, const std::vector<Detection>& detections) const;
    void request_next_frame(
        const Event& event,
        ProcessingContext& context) const;

    InferenceBackend backend_;
    std::string model_path_;
    std::string model_version_;
    std::string input_point_;
    std::string output_point_;
    std::string camera_device_id_;
    std::string camera_command_;
    std::string control_request_prefix_;
    unsigned int input_width_;
    unsigned int input_height_;
    float confidence_threshold_;
    float nms_threshold_;
    unsigned int max_detections_;
    unsigned int control_timeout_ms_;
    unsigned int onnx_intra_op_threads_;
    unsigned int onnx_inter_op_threads_;
    float onnx_input_scale_;
    bool draw_confidence_;
    unsigned int box_thickness_;
    unsigned int label_scale_;
    std::uint32_t box_color_;
    std::uint32_t text_color_;

    rknn_context context_{0};
    std::vector<std::uint8_t> model_data_;
    rknn_tensor_attr input_attr_{};
    std::vector<OutputTensor> outputs_;
    std::unique_ptr<OnnxState> onnx_state_;
    bool initialized_{false};
};

}  // namespace gateway
