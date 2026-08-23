#include "ds18b20.hpp"

#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace gateway {
namespace {

constexpr std::string_view kPluginName{"ds18b20 poll source"};
constexpr std::string_view kDefaultDevicePath{"/dev/ds18b20"};

std::string configured_path(std::string_view settings_json) {
    const auto settings = plugin_json::parse_object(settings_json, kPluginName);
    const auto path = settings.find("path");
    if (path == settings.end()) {
        return std::string{kDefaultDevicePath};
    }
    if (!path->is_string() || path->get<std::string>().empty()) {
        plugin_json::fail(kPluginName, "path", "must be a non-empty string");
    }
    return path->get<std::string>();
}

bool requested_point(const DeviceConfig& device, std::string_view point) {
    for (const auto& configured : device.points) {
        if (configured.name == point) {
            return true;
        }
    }
    return false;
}

}  // namespace

Ds18b20Driver::Ds18b20Driver(std::string device_path)
    : device_path_(std::move(device_path)) {}

DriverCapabilities Ds18b20Driver::capabilities() const {
    return DriverCapabilities{.mode = AcquisitionMode::Poll};
}

void Ds18b20Driver::configure(const DeviceConfig& device, SampleSink) {
    if (started_) {
        throw std::logic_error("cannot configure a running ds18b20 driver");
    }
    if (device.points.empty()) {
        throw std::invalid_argument("ds18b20 device must define at least one point");
    }
    for (const auto& point : device.points) {
        if (point.type != ValueType::Double) {
            throw std::invalid_argument(
                "ds18b20 point must use value type double: " + point.name);
        }
    }
    device_ = device;
    const auto connection_path = device.connection.find("path");
    if (connection_path != device.connection.end()) {
        if (connection_path->second.empty()) {
            throw std::invalid_argument("ds18b20 connection.path must not be empty");
        }
        device_path_ = connection_path->second;
    }
    configured_ = true;
}

void Ds18b20Driver::start() {
    if (!configured_) {
        throw std::logic_error("ds18b20 driver is not configured");
    }
    if (started_) {
        return;
    }

    fd_ = ::open(device_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        throw std::runtime_error(
            "cannot open DS18B20 device " + device_path_ + ": " +
            std::strerror(errno));
    }
    started_ = true;
}

void Ds18b20Driver::stop() noexcept {
    started_ = false;
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

bool Ds18b20Driver::has_point(const std::string& point) const {
    return requested_point(device_, point);
}

RawBatch Ds18b20Driver::make_status_batch(
    const CollectionGroup& group,
    Quality status,
    std::int64_t timestamp) const {
    RawBatch batch{
        .device_id = group.device_id,
        .source = "ds18b20",
        .samples = {},
    };
    batch.samples.reserve(group.points.size());
    for (const auto& point : group.points) {
        batch.samples.push_back(RawSample{
            .point = point,
            .value = 0.0,
            .status = status,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

RawBatch Ds18b20Driver::poll(
    const CollectionGroup& group,
    TimePoint deadline) {
    if (!started_) {
        throw std::logic_error("ds18b20 driver is stopped");
    }
    if (group.device_id != device_.id) {
        throw std::invalid_argument("group does not belong to ds18b20 device");
    }
    for (const auto& point : group.points) {
        if (!has_point(point)) {
            throw std::invalid_argument("unknown ds18b20 point: " + point);
        }
    }

    const auto started_at = SchedulerClock::now();
    if (started_at >= deadline) {
        return make_status_batch(group, Quality::Timeout, unix_time_ns());
    }

    double temperature = 0.0;
    ssize_t received;
    do {
        received = ::pread(fd_, &temperature, sizeof(temperature), 0);
    } while (received < 0 && errno == EINTR);

    const auto timestamp = unix_time_ns();
    if (received < 0) {
        const auto status = (errno == ENODEV || errno == ENXIO ||
                             errno == ENOENT || errno == EIO)
            ? Quality::Disconnected
            : Quality::Bad;
        return make_status_batch(group, status, timestamp);
    }
    if (received != static_cast<ssize_t>(sizeof(temperature))) {
        return make_status_batch(group, Quality::DecodeError, timestamp);
    }
    if (SchedulerClock::now() > deadline) {
        return make_status_batch(group, Quality::Timeout, timestamp);
    }

    RawBatch batch{
        .device_id = group.device_id,
        .source = "ds18b20",
        .samples = {},
    };
    batch.samples.reserve(group.points.size());
    for (const auto& point : group.points) {
        batch.samples.push_back(RawSample{
            .point = point,
            .value = temperature,
            .status = Quality::Good,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return new Ds18b20Driver(configured_path(
            settings_json == nullptr ? std::string_view{"{}"}
                                              : std::string_view{settings_json}));
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<Ds18b20Driver*>(plugin);
}

}  // namespace gateway
