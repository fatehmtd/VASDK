#pragma once

#include "../MetricsCollector.h"
#include "../MetricsConfig.h"
#include <atomic>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <functional>

namespace va {
namespace metrics {

/**
 * @brief WebSocket metrics push exporter.
 *
 * Pushes JSON snapshots to a WebSocket server at a configurable interval.
 * Clients (e.g. a dashboard) connect to the endpoint and receive periodic
 * full-snapshot JSON messages.
 *
 * Enable at CMake configure time with -DVASDK_METRICS_WEBSOCKET=ON.
 * Requires cpp-httplib (header-only) with CPPHTTPLIB_OPENSSL_SUPPORT when
 * using wss:// schemes.
 */
class WebSocketMetricsTransport : public MetricsExporterInterface {
public:
    explicit WebSocketMetricsTransport(MetricsCollector& collector,
                                        const WebSocketMetricsConfig& config = {});
    ~WebSocketMetricsTransport() override;

    WebSocketMetricsTransport(const WebSocketMetricsTransport&) = delete;
    WebSocketMetricsTransport& operator=(const WebSocketMetricsTransport&) = delete;

    void setConfig(const WebSocketMetricsConfig& config);
    const WebSocketMetricsConfig& getConfig() const { return _config; }

    /**
     * @brief Optional callback invoked when a JSON snapshot string is ready.
     * If not set, the transport handles delivery internally.
     */
    void setSnapshotCallback(std::function<void(const std::string& json)> cb);

    // MetricsExporterInterface
    void start() override;
    void stop()  override;
    void onSnapshot(const MetricsSnapshot& snapshot) override;
    void onUtteranceSummary(const MetricsSnapshot& utteranceSnapshot) override;

private:
    void pushLoop();
    std::string buildJson(const MetricsSnapshot& snap, bool isUtterance);

    MetricsCollector&           _collector;
    WebSocketMetricsConfig      _config;
    std::mutex                  _configMutex;
    std::atomic<bool>           _running{false};
    std::mutex                  _cvMutex;
    std::condition_variable     _cv;
    std::thread                 _pushThread;

    std::function<void(const std::string&)> _snapshotCallback;
    std::mutex _callbackMutex;
};

} // namespace metrics
} // namespace va
