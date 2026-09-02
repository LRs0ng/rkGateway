#pragma once

#include "gateway/acquisition.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace gateway {

class AdcPicoDriver final : public IProtocolDriver {
public:
    enum class ValueMode {
        Voltage,
        Raw,
    };

    AdcPicoDriver(
        std::string serial_device,
        std::string point,
        unsigned expected_sample_rate_hz,
        unsigned expected_batch_size,
        ValueMode value_mode,
        double vref,
        std::chrono::milliseconds read_timeout);
    ~AdcPicoDriver() override;

    [[nodiscard]] DriverCapabilities capabilities() const override;
    void configure(const DeviceConfig& device, SampleSink sink) override;
    void start() override;
    void stop() noexcept override;

private:
    struct Frame {
        std::uint32_t session_id{0};
        std::uint32_t sequence{0};
        std::uint32_t first_sample{0};
        std::uint32_t dropped_total{0};
        std::uint32_t sample_rate_hz{0};
        std::vector<std::uint16_t> samples;
    };

    void run(std::stop_token token) noexcept;
    void consume_bytes(const std::uint8_t* data, std::size_t size);
    bool try_decode_frame(Frame& frame);
    void emit_frame(const Frame& frame);
    bool open_serial();
    void close_serial() noexcept;

    static std::uint32_t crc32_with_zeroed_field(
        const std::uint8_t* data,
        std::size_t size);

    std::string serial_device_;
    std::string point_;
    unsigned expected_sample_rate_hz_{0};
    unsigned expected_batch_size_{0};
    ValueMode value_mode_{ValueMode::Voltage};
    double vref_{3.3};
    std::chrono::milliseconds read_timeout_{100};

    DeviceConfig device_;
    SampleSink sink_;
    std::jthread worker_;
    int fd_{-1};
    std::vector<std::uint8_t> input_buffer_;
    std::atomic_bool configured_{false};
    std::atomic_bool started_{false};
};

}  // namespace gateway
