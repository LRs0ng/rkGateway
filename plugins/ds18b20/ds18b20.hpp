#pragma once

#include "gateway/acquisition.hpp"

#include <chrono>
#include <string>

namespace gateway {

class Ds18b20Driver final : public IProtocolDriver {
public:
    explicit Ds18b20Driver(std::string device_path);

    [[nodiscard]] DriverCapabilities capabilities() const override;
    void configure(const DeviceConfig& device, SampleSink sink) override;
    void start() override;
    void stop() noexcept override;
    [[nodiscard]] RawBatch poll(
        const CollectionGroup& group,
        TimePoint deadline) override;

private:
    [[nodiscard]] RawBatch make_status_batch(
        const CollectionGroup& group,
        Quality status,
        std::int64_t timestamp) const;
    [[nodiscard]] bool has_point(const std::string& point) const;

    std::string device_path_;
    DeviceConfig device_;
    int fd_{-1};
    bool configured_{false};
    bool started_{false};
};

}  // namespace gateway
