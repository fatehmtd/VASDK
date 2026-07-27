#include "VASDK/metrics/MetricsReporter.h"
#include "VASDK/Transport.h"
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace va {
namespace metrics {

MetricsReporter::MetricsReporter(MetricsCollector& collector, const ReporterConfig& config)
    : _collector(collector), _config(config)
{}

MetricsReporter::~MetricsReporter()
{
    stop();
}

void MetricsReporter::setConfig(const ReporterConfig& config)
{
    std::lock_guard<std::mutex> lk(_configMutex);
    _config = config;
}

void MetricsReporter::start()
{
    _running.store(true);
    _periodicThread = std::thread(&MetricsReporter::periodicLoop, this);
}

void MetricsReporter::stop()
{
    _running.store(false);
    _cv.notify_one();
    if (_periodicThread.joinable()) _periodicThread.join();
}

void MetricsReporter::onUtteranceEnd(uint64_t utteranceStartNs) noexcept
{
    printUtteranceSummary(utteranceStartNs);
}

void MetricsReporter::addExporter(std::shared_ptr<MetricsExporterInterface> exporter)
{
    std::lock_guard<std::mutex> lk(_exporterMutex);
    _exporters.push_back(std::move(exporter));
}

void MetricsReporter::addTransport(std::shared_ptr<va::transport::Transport> transport)
{
    std::lock_guard<std::mutex> lk(_exporterMutex);
    _transports.push_back(std::move(transport));
}

void MetricsReporter::periodicLoop()
{
    while (_running.load()) {
        int intervalSecs;
        bool doPeriodic;
        {
            std::lock_guard<std::mutex> lk(_configMutex);
            intervalSecs = _config.intervalSeconds;
            doPeriodic   = _config.enablePeriodicReport;
        }

        if (intervalSecs <= 0) {
            // Periodic reporting disabled - park the thread until stopped.
            std::unique_lock<std::mutex> lk(_cvMutex);
            _cv.wait(lk, [this] { return !_running.load(); });
            break;
        }

        {
            std::unique_lock<std::mutex> lk(_cvMutex);
            _cv.wait_for(lk, std::chrono::seconds(intervalSecs),
                         [this] { return !_running.load(); });
        }

        if (!_running.load()) break;

        if (doPeriodic) printNow();
    }
}

void MetricsReporter::printNow()
{
    auto snap = _collector.snapshot();

    printSnapshot(snap, "=== VASDK Metrics Report ===", false, 0);

    // Transport stats (separate from the collector snapshot).
    {
        std::lock_guard<std::mutex> lk(_exporterMutex);
        bool doTransport;
        {
            std::lock_guard<std::mutex> lk2(_configMutex);
            doTransport = _config.enableTransportGroup;
        }
        if (doTransport && !_transports.empty()) {
            spdlog::info("  -- Transports --");
            for (auto& t : _transports) {
                if (!t) continue;
                auto s = t->getStats();
                spdlog::info("  [{}] rx={} tx={} bytes_rx={} bytes_tx={} err={} dropped={}",
                    t->getName(),
                    s.framesReceived, s.framesSent,
                    s.bytesReceived,  s.bytesSent,
                    s.errors,         s.droppedFrames);
            }
        }
    }

    // Forward to attached exporters.
    {
        std::lock_guard<std::mutex> lk(_exporterMutex);
        for (auto& exp : _exporters) {
            if (exp) exp->onSnapshot(snap);
        }
    }
}

void MetricsReporter::printUtteranceSummary(uint64_t utteranceStartNs)
{
    bool doSummary;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        doSummary = _config.enableUtteranceSummary;
    }
    if (!doSummary) return;

    auto snap = _collector.snapshot();

    // Build an utterance-scoped snapshot: filter each sampler to recent samples only.
    MetricsSnapshot utterSnap;
    utterSnap.capturedAtNs = snap.capturedAtNs;
    utterSnap.counters      = snap.counters;
    utterSnap.gauges        = snap.gauges;

    for (auto& ss : snap.samplers) {
        SamplerSnapshot filtered = ss;
        // Keep only samples since utterance started.
        filtered.samples.erase(
            std::remove_if(filtered.samples.begin(), filtered.samples.end(),
                [utteranceStartNs](const SlidingWindowSampler::Sample& s) {
                    return s.timestampNs < utteranceStartNs;
                }),
            filtered.samples.end());

        // Recompute mean + quantiles from filtered set.
        if (!filtered.samples.empty()) {
            double sum = 0.0;
            for (auto& s : filtered.samples) sum += s.value;
            filtered.mean = sum / static_cast<double>(filtered.samples.size());

            std::vector<double> vals;
            vals.reserve(filtered.samples.size());
            for (auto& s : filtered.samples) vals.push_back(s.value);
            std::sort(vals.begin(), vals.end());

            filtered.quantileValues.clear();
            for (double q : filtered.quantiles) {
                size_t idx = static_cast<size_t>(q * (vals.size() - 1));
                filtered.quantileValues.push_back(vals[std::min(idx, vals.size() - 1)]);
            }
        }

        if (!filtered.samples.empty()) utterSnap.samplers.push_back(std::move(filtered));
    }

    printSnapshot(utterSnap, "=== Utterance Summary ===", true, utteranceStartNs);

    std::lock_guard<std::mutex> lk(_exporterMutex);
    for (auto& exp : _exporters) {
        if (exp) exp->onUtteranceSummary(utterSnap);
    }
}

void MetricsReporter::printSnapshot(const MetricsSnapshot&, const char* header,
                                    bool utteranceOnly, uint64_t /*sinceNs*/)
{
    auto snap = _collector.snapshot();

    ReporterConfig cfg;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        cfg = _config;
    }

    spdlog::info("{}", header);

    if (cfg.enableProcessorGroup || cfg.enableONNXGroup || cfg.enableQualityGroup) {
        for (auto& ss : snap.samplers) {
            if (ss.totalObserved == 0) continue;
            std::string qStr;
            for (size_t i = 0; i < ss.quantiles.size(); ++i) {
                if (i) qStr += " ";
                char buf[64];
                std::snprintf(buf, sizeof(buf), "p%.0f=%.2fms",
                    ss.quantiles[i] * 100.0, ss.quantileValues.empty() ? 0.0 : ss.quantileValues[i]);
                qStr += buf;
            }
            if (ss.droppedCount > 0) {
                spdlog::info("  {} n={} mean={:.2f}ms {} [WARN: {} dropped]",
                    ss.name, ss.totalObserved, ss.mean, qStr, ss.droppedCount);
            } else {
                spdlog::info("  {} n={} mean={:.2f}ms {}", ss.name, ss.totalObserved, ss.mean, qStr);
            }
        }
    }

    if (cfg.enableAgentStateGroup || cfg.enableTransportGroup) {
        for (auto& cs : snap.counters) {
            spdlog::info("  {} = {}", cs.name, cs.value);
        }
    }

    for (auto& gs : snap.gauges) {
        spdlog::info("  {} = {:.4f}", gs.name, gs.value);
    }
}

} // namespace metrics
} // namespace va
