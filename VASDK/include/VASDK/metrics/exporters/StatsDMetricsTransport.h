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
 * @brief StatsD UDP metrics exporter.
 *
 * Periodically flushes counters, gauges, and sampler percentiles to a StatsD-compatible
 * server (e.g. Telegraf, Datadog agent, statsd). Uses gauge_set for absolute values and
 * increment-based counters so the server-side derivative gives per-interval rates.
 *
 * Enable at CMake configure time with -DVASDK_METRICS_STATSD=ON.
 */
class StatsDMetricsTransport : public MetricsExporterInterface {
public:
    explicit StatsDMetricsTransport(MetricsCollector& collector, const StatsDConfig& config = {});
    ~StatsDMetricsTransport() override;

    StatsDMetricsTransport(const StatsDMetricsTransport&) = delete;
    StatsDMetricsTransport& operator=(const StatsDMetricsTransport&) = delete;

    void setConfig(const StatsDConfig& config);
    const StatsDConfig& getConfig() const { return _config; }

    // MetricsExporterInterface
    void start() override;
    void stop()  override;
    void onSnapshot(const MetricsSnapshot& snapshot) override;

private:
    void flushLoop();
    void sendSnapshot(const MetricsSnapshot& snapshot);
    void sendUDP(const std::string& payload);

    MetricsCollector&   _collector;
    StatsDConfig        _config;
    std::mutex          _configMutex;
    std::atomic<bool>   _running{false};
    std::mutex          _cvMutex;
    std::condition_variable _cv;
    std::thread         _flushThread;
    int                 _sockFd{-1};

    // Cache last counter values for delta-based StatsD counters
    struct CounterCache { uint64_t last{0}; };
    std::vector<std::pair<std::string, CounterCache>> _counterCache;
    std::mutex _cacheMutex;
};

} // namespace metrics
} // namespace va
