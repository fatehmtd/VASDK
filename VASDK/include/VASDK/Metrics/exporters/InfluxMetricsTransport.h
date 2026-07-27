#pragma once

#include "../MetricsCollector.h"
#include "../MetricsConfig.h"
#include <atomic>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <vector>
#include <string>

namespace va {
namespace metrics {

/**
 * @brief InfluxDB v2 Line Protocol HTTP metrics exporter.
 *
 * Batches measurements and flushes them to InfluxDB v2's /api/v2/write endpoint.
 * Each metric becomes an InfluxDB measurement named after the metric key, with a
 * "host" tag and optional user tags from InfluxConfig.
 *
 * Enable at CMake configure time with -DVASDK_METRICS_INFLUX=ON.
 */
class InfluxMetricsTransport : public MetricsExporterInterface {
public:
    explicit InfluxMetricsTransport(MetricsCollector& collector, const InfluxConfig& config = {});
    ~InfluxMetricsTransport() override;

    InfluxMetricsTransport(const InfluxMetricsTransport&) = delete;
    InfluxMetricsTransport& operator=(const InfluxMetricsTransport&) = delete;

    void setConfig(const InfluxConfig& config);
    const InfluxConfig& getConfig() const { return _config; }

    // MetricsExporterInterface
    void start() override;
    void stop()  override;
    void onSnapshot(const MetricsSnapshot& snapshot) override;
    void onUtteranceSummary(const MetricsSnapshot& utteranceSnapshot) override;

private:
    void flushLoop();
    void flush();
    void appendLine(const std::string& measurement, const std::string& fields, uint64_t timestampNs);
    bool httpPost(const std::string& body);

    MetricsCollector&       _collector;
    InfluxConfig            _config;
    std::mutex              _configMutex;
    std::atomic<bool>       _running{false};
    std::mutex              _cvMutex;
    std::condition_variable _cv;
    std::thread             _flushThread;

    std::vector<std::string>    _pendingLines;
    std::mutex                  _linesMutex;
};

} // namespace metrics
} // namespace va
