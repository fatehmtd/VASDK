#pragma once

#include "MetricsCollector.h"
#include "MetricsConfig.h"
#include "UtteranceObserver.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace va {
namespace transport { class Transport; }

namespace metrics {

/**
 * Periodic and per-utterance metrics reporter.
 *
 * Calls printNow() on a background thread every ReporterConfig::intervalSeconds.
 * Implements UtteranceObserverInterface so AgentStateProcessor depends only on
 * the abstract interface, not this class directly.
 * Attached MetricsExporterInterface instances receive snapshots for remote export.
 */
class VASDK_EXPORT MetricsReporter
    : public MetricsExporterInterface
    , public UtteranceObserverInterface {
public:
    MetricsReporter(MetricsCollector& collector, const ReporterConfig& config = {});
    ~MetricsReporter() override;

    MetricsReporter(const MetricsReporter&) = delete;
    MetricsReporter& operator=(const MetricsReporter&) = delete;

    void setConfig(const ReporterConfig& config);
    const ReporterConfig& getConfig() const { return _config; }

    // MetricsExporterInterface
    void start() override;
    void stop()  override;
    void onSnapshot(const MetricsSnapshot& snapshot) override {}
    void onUtteranceSummary(const MetricsSnapshot& utteranceSnapshot) override {}

    // UtteranceObserverInterface
    void onUtteranceEnd(uint64_t utteranceStartNs) noexcept override;

    /** Full session report - logs via spdlog at _config.logLevel. */
    void printNow();

    /**
     * @brief Per-utterance latency + quality report.
     * @param utteranceStartNs  steady_clock nanoseconds at utterance start (LISTENING entry).
     */
    void printUtteranceSummary(uint64_t utteranceStartNs);

    /** @brief Attach an exporter that receives snapshots and utterance summaries. */
    void addExporter(std::shared_ptr<MetricsExporterInterface> exporter);

    /** @brief Include this transport's getStats() in each periodic report. */
    void addTransport(std::shared_ptr<va::transport::Transport> transport);

private:
    void periodicLoop();
    void printSnapshot(const MetricsSnapshot& snap, const char* header, bool utteranceOnly,
                       uint64_t sinceNs = 0);

    MetricsCollector&               _collector;
    ReporterConfig                  _config;
    mutable std::mutex              _configMutex;
    std::atomic<bool>               _running{false};
    std::mutex                      _cvMutex;
    std::condition_variable         _cv;
    std::thread                     _periodicThread;

    std::vector<std::shared_ptr<MetricsExporterInterface>>  _exporters;
    std::vector<std::shared_ptr<va::transport::Transport>>  _transports;
    std::mutex                                              _exporterMutex;
};

} // namespace metrics
} // namespace va
