#pragma once

// Prometheus exporter - compiled only when VASDK_METRICS_PROMETHEUS=ON
// Exposes a /metrics HTTP endpoint in Prometheus text exposition format.
//
// Requires cpp-httplib (fetched automatically by CMake when the option is ON).
// The server binds on PrometheusConfig::bindAddress:port.
// Counters and Gauge snapshots are exposed as-is; SlidingWindowSampler
// percentiles (p50, p90, p99) are exposed as gauge-typed metrics.

#include "../MetricsCollector.h"
#include "../MetricsConfig.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <memory>

// Forward-declare the httplib server to avoid pulling in the header in all TUs.
namespace httplib { class Server; }

namespace va {
namespace metrics {

class PrometheusExporter : public MetricsExporterInterface {
public:
    explicit PrometheusExporter(MetricsCollector& collector, const PrometheusConfig& config = {});
    ~PrometheusExporter() override;

    PrometheusExporter(const PrometheusExporter&) = delete;
    PrometheusExporter& operator=(const PrometheusExporter&) = delete;

    void setConfig(const PrometheusConfig& config);
    const PrometheusConfig& getConfig() const { return _config; }

    // MetricsExporterInterface
    void start() override;
    void stop()  override;
    void onSnapshot(const MetricsSnapshot& snapshot) override {}  // pull-based: ignored

private:
    std::string buildPage();
    void serveLoop();

    MetricsCollector&           _collector;
    PrometheusConfig            _config;
    mutable std::mutex          _configMutex;
    std::atomic<bool>           _running{false};
    std::thread                 _serverThread;
    std::unique_ptr<httplib::Server> _server;
};

} // namespace metrics
} // namespace va
