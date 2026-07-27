#pragma once

#include "MetricsConfig.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>

namespace va {
namespace metrics {

//  Forward declarations 

class MetricsCollector;
struct MetricsSnapshot;

//  MetricBase 

/** Abstract base for all metric types stored in the registry. */
class MetricBase {
public:
    virtual ~MetricBase() = default;
    const std::string& name() const { return _name; }

protected:
    explicit MetricBase(std::string name) : _name(std::move(name)) {}

private:
    std::string _name;
};

//  Counter 

/**
 * Monotonically increasing counter backed by a single atomic.
 *
 * increment() and add() are lock-free; value() loads with relaxed ordering
 * since exact cross-thread visibility during reporting is not required.
 */
class Counter : public MetricBase {
public:
    explicit Counter(std::string name) : MetricBase(std::move(name)) {}

    void increment() noexcept { _value.fetch_add(1, std::memory_order_relaxed); }
    void add(uint64_t n) noexcept { _value.fetch_add(n, std::memory_order_relaxed); }
    uint64_t value() const noexcept { return _value.load(std::memory_order_relaxed); }
    void reset() noexcept { _value.store(0, std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> _value{0};
};

//  AlertDispatcher 

/**
 * Bounded SPSC queue that delivers threshold alerts on a background thread.
 *
 * Audio threads write one atomic CAS and one store when a threshold fires.
 * The background thread reads and dispatches - callers are never blocked.
 * When the queue is full, the alert is dropped silently.
 */
class AlertDispatcher {
public:
    using Handler = std::function<void(const std::string& metricName, double value)>;

    explicit AlertDispatcher(const AlertDispatcherConfig& cfg);
    ~AlertDispatcher();

    // Non-copyable, non-movable - handlers hold raw pointers to this.
    AlertDispatcher(const AlertDispatcher&) = delete;
    AlertDispatcher& operator=(const AlertDispatcher&) = delete;

    /** Post an alert onto the queue; called from hot path - never blocks. */
    void post(const std::string& metricName, double value) noexcept;

    /** Register a handler invoked on the background thread. */
    void addHandler(Handler h);

    void start();
    void stop();

private:
    struct Alert {
        std::string metricName;
        double      value;
    };

    void dispatchLoop();

    const AlertDispatcherConfig  _config;
    std::vector<Alert>           _ring;       // fixed-size ring buffer
    std::atomic<size_t>          _head{0};    // producer writes here
    std::atomic<size_t>          _tail{0};    // consumer reads here
    std::vector<Handler>         _handlers;
    std::mutex                   _handlerMutex;
    std::atomic<bool>            _running{false};
    std::mutex                   _cvMutex;
    std::condition_variable      _cv;
    std::thread                  _thread;
};

//  Gauge 

/**
 * Floating-point gauge backed by an atomic double (bit-cast trick for lock-free store/load).
 *
 * setThreshold() registers a threshold that, when exceeded, posts a single alert
 * to the AlertDispatcher. The alert fires once per crossing (edge-triggered).
 */
class VASDK_EXPORT Gauge : public MetricBase {
public:
    Gauge(std::string name, AlertDispatcher* dispatcher);

    void set(double v) noexcept;
    double value() const noexcept;

    /**
     * @brief Register an alert threshold. Fires via AlertDispatcher when value crosses above threshold.
     * @param threshold  Value that triggers the alert when exceeded.
     * @param dispatcher Must outlive this Gauge.
     */
    void setThreshold(double threshold, AlertDispatcher* dispatcher);

private:
    // std::atomic<double> is not always lock-free; use uint64_t bit-cast.
    std::atomic<uint64_t> _bits{0};
    double        _threshold{0.0};
    bool          _hasThreshold{false};
    bool          _aboveThreshold{false};  // edge-detect state
    AlertDispatcher* _dispatcher{nullptr};
    std::string   _name;                   // cached for alert post
};

//  SlidingWindowSampler 

/**
 * Lock-free MPSC ring buffer of timestamped double observations.
 *
 * observe() = 1 atomic fetch_add (sequence) + 1 release store - no mutex.
 * percentile() copies the window and sorts, so the O(N log N) cost is paid
 * only at report time. recentSamples() filters by steady_clock nanoseconds
 * for per-utterance views.
 */
class VASDK_EXPORT SlidingWindowSampler : public MetricBase {
public:
    struct Sample {
        double   value;
        uint64_t timestampNs;  // steady_clock nanoseconds - NOT system_clock
    };

    SlidingWindowSampler(std::string name, const SamplerConfig& cfg);

    /** Lock-free observation. Drops oldest slot when window is full (ring). */
    void observe(double value) noexcept;

    /** @brief p-th percentile of all values in the current window. p in [0,1]. */
    double percentile(double p) const;

    /** @brief Mean of all values in the current window. */
    double mean() const;

    /** @brief Observations since sinceNs (steady_clock nanoseconds). */
    std::vector<Sample> recentSamples(uint64_t sinceNs) const;

    /** @brief All samples currently in the window. */
    std::vector<Sample> allSamples() const;

    size_t   windowSize()    const noexcept { return _mask + 1; }
    uint64_t totalObserved() const noexcept { return _total.load(std::memory_order_relaxed); }

    /** Samples that have wrapped off the back of the window. */
    uint64_t droppedCount() const noexcept {
        uint64_t t = totalObserved();
        size_t   w = windowSize();
        return t > w ? t - w : 0;
    }

    bool hasSaturated() const noexcept { return totalObserved() >= windowSize(); }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};   // sequence; odd = being written, even = ready
        double                value{0.0};
        uint64_t              timestampNs{0};
    };

    std::vector<Slot>     _slots;
    size_t                _mask;         // windowSize - 1 for cheap modulo
    std::atomic<uint64_t> _total{0};     // ever-increasing observation count
};

//  ScopedTimer 

/**
 * RAII timer using steady_clock. Calls sampler.observe(elapsed_ms) on destruct.
 * Zero overhead when sampler is null (disabled metrics path).
 */
class VASDK_EXPORT ScopedTimer {
public:
    explicit ScopedTimer(SlidingWindowSampler* sampler) noexcept
        : _sampler(sampler)
        , _start(sampler ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
    {}

    ~ScopedTimer() {
        if (!_sampler) return;
        auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - _start).count();
        _sampler->observe(elapsed);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    SlidingWindowSampler*                    _sampler;
    std::chrono::steady_clock::time_point    _start;
};

//  MetricDescriptor 

enum class MetricType { Counter, Gauge, Sampler };

/**
 * Compile-time descriptor identifying a metric.
 *
 * windowSize = 0 means "use CollectorConfig::defaultSampler.windowSize".
 * name is the base name; namePrefix from CollectorConfig is prepended at registration.
 */
struct MetricDescriptor {
    const char* name;
    MetricType  type;
    const char* help;
    size_t      windowSize = 0;  // Sampler only; 0 = collector default
};

//  Snapshot 

struct CounterSnapshot {
    std::string name;
    uint64_t    value;
};

struct GaugeSnapshot {
    std::string name;
    double      value;
};

struct SamplerSnapshot {
    std::string          name;
    uint64_t             totalObserved;
    uint64_t             droppedCount;
    double               mean;
    std::vector<double>  quantiles;         // one per SamplerConfig::quantiles
    std::vector<double>  quantileValues;    // parallel to quantiles
    std::vector<SlidingWindowSampler::Sample> samples;  // all window samples
};

struct MetricsSnapshot {
    std::vector<CounterSnapshot> counters;
    std::vector<GaugeSnapshot>   gauges;
    std::vector<SamplerSnapshot> samplers;
    uint64_t                     capturedAtNs;  // steady_clock nanoseconds
};

//  TimingObserverInterface 

/**
 * Injected into 3P inference providers so they can report per-stage latency
 * without depending on any VASDK metric type.
 *
 * stage is a short string like "encoder", "decoder", "synthesis_chunk".
 * elapsedMs is measured with steady_clock.
 */
class TimingObserverInterface {
public:
    virtual ~TimingObserverInterface() = default;
    virtual void onTiming(const char* stage, double elapsedMs) noexcept = 0;
};

//  MetricsExporterInterface 

/**
 * Base for all exporters (reporter, Prometheus, StatsD, Influx, WebSocket).
 *
 * MetricsReporter calls onSnapshot() at each interval and onUtteranceSummary()
 * on each turn end.
 */
class MetricsExporterInterface {
public:
    virtual ~MetricsExporterInterface() = default;
    virtual void start() {}
    virtual void stop()  {}
    virtual void onSnapshot(const MetricsSnapshot& snapshot) = 0;
    virtual void onUtteranceSummary(const MetricsSnapshot& utteranceSnapshot) {}
};

//  MetricsCollector 

/**
 * Central registry for all metrics.
 *
 * get<T>(desc) is called once per processor in initialize() and returns a raw
 * pointer valid for the life of the collector. Hot-path observation calls hold
 * that pointer directly - no registry lookup, no lock.
 *
 * The shared_mutex is only locked during registration (write) and snapshot
 * (read). Both are off the hot path.
 */
class VASDK_EXPORT MetricsCollector {
public:
    explicit MetricsCollector(const CollectorConfig& config = {});
    ~MetricsCollector();

    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;

    /** Global singleton - used when processors are constructed without an injected collector. */
    static MetricsCollector& instance();

    /**
     * @brief Configure the singleton before its first use.
     *
     * Must be called before any processor is constructed (i.e. before the first
     * call to instance()). Has no effect after the singleton has been created.
     * This allows application code to pass the full CollectorConfig (e.g. from
     * CLI / JSON) to the singleton, including the enabled flag and namePrefix.
     */
    static void setInstanceConfig(const CollectorConfig& config);

    const CollectorConfig& config() const { return _config; }

    /**
     * @brief Look up or register a metric of type T.
     *
     * First call creates and stores the metric. Subsequent calls return the
     * same pointer. windowSize = 0 in desc is resolved to
     * _config.defaultSampler.windowSize.
     *
     * @return nullptr if _config.enabled is false.
     */
    template<typename T>
    T* get(const MetricDescriptor& desc);

    /** Build a full snapshot of all registered metrics. Acquires a shared lock. */
    MetricsSnapshot snapshot() const;

    AlertDispatcher* alertDispatcher() { return _alertDispatcher.get(); }

private:
    std::string qualifiedName(const MetricDescriptor& desc) const;

    CollectorConfig                                   _config;
    std::unordered_map<std::string,
        std::unique_ptr<MetricBase>>                  _registry;
    mutable std::shared_mutex                         _registryMutex;
    std::unique_ptr<AlertDispatcher>                  _alertDispatcher;
};

//  Template definitions 

template<typename T>
T* MetricsCollector::get(const MetricDescriptor& desc)
{
    if (!_config.enabled) return nullptr;

    std::string key = qualifiedName(desc);

    // Fast path: already registered (shared lock).
    {
        std::shared_lock<std::shared_mutex> rlock(_registryMutex);
        auto it = _registry.find(key);
        if (it != _registry.end()) {
            return static_cast<T*>(it->second.get());
        }
    }

    // Slow path: register (exclusive lock).
    std::unique_lock<std::shared_mutex> wlock(_registryMutex);
    // Re-check after upgrade (another thread may have inserted while we waited).
    auto it = _registry.find(key);
    if (it != _registry.end()) return static_cast<T*>(it->second.get());

    std::unique_ptr<T> metric;
    if constexpr (std::is_same_v<T, Counter>) {
        metric = std::make_unique<Counter>(key);
    } else if constexpr (std::is_same_v<T, Gauge>) {
        metric = std::make_unique<Gauge>(key, _alertDispatcher.get());
    } else if constexpr (std::is_same_v<T, SlidingWindowSampler>) {
        SamplerConfig cfg = _config.defaultSampler;
        if (desc.windowSize != 0) cfg.windowSize = desc.windowSize;
        metric = std::make_unique<SlidingWindowSampler>(key, cfg);
    }

    T* ptr = metric.get();
    _registry.emplace(key, std::move(metric));
    return ptr;
}

} // namespace metrics
} // namespace va
