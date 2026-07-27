// WebSocketMetricsTransport.cpp - compiled only when VASDK_METRICS_WEBSOCKET=ON
//
// Delivers JSON metric snapshots via a user-provided callback.
// If no callback is set, snapshots are logged at debug level.
//
// For integration with a WebSocket library (e.g. libwebsockets, uWebSockets),
// register a callback via setSnapshotCallback() and forward the JSON string to
// your WebSocket broadcast function.

#include <VASDK/metrics/exporters/WebSocketMetricsTransport.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <condition_variable>
#include <sstream>
#include <thread>

namespace va {
namespace metrics {

WebSocketMetricsTransport::WebSocketMetricsTransport(MetricsCollector& collector,
                                                     const WebSocketMetricsConfig& config)
    : _collector(collector)
    , _config(config)
{}

WebSocketMetricsTransport::~WebSocketMetricsTransport()
{
    stop();
}

void WebSocketMetricsTransport::setConfig(const WebSocketMetricsConfig& config)
{
    std::lock_guard<std::mutex> lk(_configMutex);
    _config = config;
}

void WebSocketMetricsTransport::setSnapshotCallback(
    std::function<void(const std::string& json)> cb)
{
    std::lock_guard<std::mutex> lk(_callbackMutex);
    _snapshotCallback = std::move(cb);
}

void WebSocketMetricsTransport::start()
{
    if (_running.exchange(true)) return;
    _pushThread = std::thread(&WebSocketMetricsTransport::pushLoop, this);
    spdlog::debug("WebSocketMetricsTransport started (pushIntervalMs={})",
                  _config.pushIntervalMs);
}

void WebSocketMetricsTransport::stop()
{
    if (!_running.exchange(false)) return;
    _cv.notify_one();
    if (_pushThread.joinable()) _pushThread.join();
}

void WebSocketMetricsTransport::onSnapshot(const MetricsSnapshot& snapshot)
{
    auto json = buildJson(snapshot, false);
    std::lock_guard<std::mutex> lk(_callbackMutex);
    if (_snapshotCallback) {
        _snapshotCallback(json);
    } else {
        spdlog::debug("WebSocketMetricsTransport snapshot: {} bytes", json.size());
    }
}

void WebSocketMetricsTransport::onUtteranceSummary(const MetricsSnapshot& utteranceSnapshot)
{
    auto json = buildJson(utteranceSnapshot, true);
    std::lock_guard<std::mutex> lk(_callbackMutex);
    if (_snapshotCallback) {
        _snapshotCallback(json);
    }
}

//  Private 

void WebSocketMetricsTransport::pushLoop()
{
    int intervalMs;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        intervalMs = _config.pushIntervalMs;
    }
    if (intervalMs <= 0) return; // push-on-event mode only

    while (_running) {
        {
            std::unique_lock<std::mutex> lk(_cvMutex);
            _cv.wait_for(lk, std::chrono::milliseconds(intervalMs),
                         [this] { return !_running.load(); });
        }
        if (!_running) break;
        auto snap = _collector.snapshot();
        onSnapshot(snap);
    }
}

std::string WebSocketMetricsTransport::buildJson(const MetricsSnapshot& snap, bool isUtterance)
{
    // Minimal hand-rolled JSON - avoids requiring nlohmann_json at this layer
    std::ostringstream o;
    o << "{\"type\":\"" << (isUtterance ? "utterance" : "snapshot") << "\","
      << "\"ts\":" << snap.capturedAtNs << ","
      << "\"counters\":[";
    for (size_t i = 0; i < snap.counters.size(); ++i) {
        if (i) o << ',';
        o << "{\"name\":\"" << snap.counters[i].name << "\","
          << "\"value\":" << snap.counters[i].value << "}";
    }
    o << "],\"gauges\":[";
    for (size_t i = 0; i < snap.gauges.size(); ++i) {
        if (i) o << ',';
        o << "{\"name\":\"" << snap.gauges[i].name << "\","
          << "\"value\":" << snap.gauges[i].value << "}";
    }
    o << "],\"samplers\":[";
    for (size_t i = 0; i < snap.samplers.size(); ++i) {
        const auto& s = snap.samplers[i];
        if (i) o << ',';
        o << "{\"name\":\"" << s.name << "\","
          << "\"mean\":" << s.mean << ","
          << "\"total\":" << s.totalObserved << ","
          << "\"quantiles\":{";
        for (size_t q = 0; q < s.quantiles.size() && q < s.quantileValues.size(); ++q) {
            if (q) o << ',';
            std::string pLabel = "p" + std::to_string(static_cast<int>(s.quantiles[q] * 100));
            o << "\"" << pLabel << "\":" << s.quantileValues[q];
        }
        o << "}}";
    }
    o << "]}";
    return o.str();
}

} // namespace metrics
} // namespace va
