#include "VASDK/metrics/MetricsCollector.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <numeric>
#include <thread>

namespace va {
namespace metrics {

//  AlertDispatcher 

AlertDispatcher::AlertDispatcher(const AlertDispatcherConfig& cfg)
    : _config(cfg)
    , _ring(cfg.queueCapacity)
{}

AlertDispatcher::~AlertDispatcher()
{
    stop();
}

void AlertDispatcher::start()
{
    if (!_config.enabled) return;
    _running.store(true);
    _thread = std::thread(&AlertDispatcher::dispatchLoop, this);
}

void AlertDispatcher::stop()
{
    _running.store(false);
    _cv.notify_one();
    if (_thread.joinable()) _thread.join();
}

void AlertDispatcher::addHandler(Handler h)
{
    std::lock_guard<std::mutex> lk(_handlerMutex);
    _handlers.push_back(std::move(h));
}

void AlertDispatcher::post(const std::string& metricName, double value) noexcept
{
    if (!_config.enabled) return;

    size_t h = _head.load(std::memory_order_relaxed);
    size_t t = _tail.load(std::memory_order_acquire);

    // Drop when full.
    if (h - t >= _ring.size()) return;

    _ring[h % _ring.size()] = {metricName, value};
    _head.store(h + 1, std::memory_order_release);
    _cv.notify_one();
}

void AlertDispatcher::dispatchLoop()
{
    while (true) {
        size_t h = _head.load(std::memory_order_acquire);
        size_t t = _tail.load(std::memory_order_relaxed);

        if (t == h) {
            std::unique_lock<std::mutex> lk(_cvMutex);
            _cv.wait(lk, [this] {
                return _head.load(std::memory_order_acquire)
                           != _tail.load(std::memory_order_relaxed)
                       || !_running.load(std::memory_order_relaxed);
            });
            if (!_running.load(std::memory_order_relaxed)
                && _head.load(std::memory_order_acquire)
                       == _tail.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        }

        const Alert& a = _ring[t % _ring.size()];
        {
            std::lock_guard<std::mutex> lk(_handlerMutex);
            for (auto& handler : _handlers) handler(a.metricName, a.value);
        }
        _tail.store(t + 1, std::memory_order_release);

        if (!_running.load(std::memory_order_relaxed)
            && _head.load(std::memory_order_acquire)
                   == _tail.load(std::memory_order_relaxed)) {
            break;
        }
    }
}

//  Gauge 

static double bitsToDouble(uint64_t bits) noexcept
{
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

static uint64_t doubleToBits(double d) noexcept
{
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

Gauge::Gauge(std::string name, AlertDispatcher* dispatcher)
    : MetricBase(name), _name(std::move(name)), _dispatcher(dispatcher)
{}

void Gauge::set(double v) noexcept
{
    _bits.store(doubleToBits(v), std::memory_order_relaxed);

    if (_hasThreshold && _dispatcher) {
        bool above = v > _threshold;
        if (above && !_aboveThreshold) {
            // Rising edge - fire once.
            _aboveThreshold = true;
            _dispatcher->post(_name, v);
        } else if (!above) {
            _aboveThreshold = false;
        }
    }
}

double Gauge::value() const noexcept
{
    return bitsToDouble(_bits.load(std::memory_order_relaxed));
}

void Gauge::setThreshold(double threshold, AlertDispatcher* dispatcher)
{
    _threshold    = threshold;
    _hasThreshold = true;
    _dispatcher   = dispatcher;
}

//  SlidingWindowSampler 

// Round up to next power of two so modulo becomes a cheap bitwise AND.
static size_t nextPow2(size_t n)
{
    if (n == 0) return 1;
    // std::bit_ceil is C++20; use a manual fallback for C++17 compatibility.
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

SlidingWindowSampler::SlidingWindowSampler(std::string name, const SamplerConfig& cfg)
    : MetricBase(std::move(name))
    , _slots(nextPow2(cfg.windowSize))
    , _mask(_slots.size() - 1)
{}

void SlidingWindowSampler::observe(double value) noexcept
{
    // Sequence number written after data so the reader can detect a clean slot.
    uint64_t idx    = _total.fetch_add(1, std::memory_order_relaxed);
    size_t   slot   = static_cast<size_t>(idx & _mask);
    auto&    s      = _slots[slot];

    // Mark in-progress (odd seq).
    s.seq.store(idx * 2 + 1, std::memory_order_relaxed);
    s.value       = value;
    s.timestampNs = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    // Mark complete (even seq) - release ensures consumer sees consistent data.
    s.seq.store(idx * 2 + 2, std::memory_order_release);
}

std::vector<SlidingWindowSampler::Sample> SlidingWindowSampler::allSamples() const
{
    std::vector<Sample> out;
    out.reserve(_slots.size());

    for (size_t i = 0; i < _slots.size(); ++i) {
        uint64_t seq1 = _slots[i].seq.load(std::memory_order_acquire);
        if (seq1 == 0) continue;  // never written
        double   v  = _slots[i].value;
        uint64_t ts = _slots[i].timestampNs;
        uint64_t seq2 = _slots[i].seq.load(std::memory_order_acquire);
        // Skip if slot is being written (odd seq) or torn (seq changed).
        if (seq1 != seq2 || (seq1 & 1)) continue;
        out.push_back({v, ts});
    }
    return out;
}

double SlidingWindowSampler::percentile(double p) const
{
    auto samples = allSamples();
    if (samples.empty()) return 0.0;
    std::vector<double> vals;
    vals.reserve(samples.size());
    for (auto& s : samples) vals.push_back(s.value);
    std::sort(vals.begin(), vals.end());
    size_t idx = static_cast<size_t>(p * (vals.size() - 1));
    return vals[std::min(idx, vals.size() - 1)];
}

double SlidingWindowSampler::mean() const
{
    auto samples = allSamples();
    if (samples.empty()) return 0.0;
    double sum = 0.0;
    for (auto& s : samples) sum += s.value;
    return sum / static_cast<double>(samples.size());
}

std::vector<SlidingWindowSampler::Sample>
SlidingWindowSampler::recentSamples(uint64_t sinceNs) const
{
    auto all = allSamples();
    std::vector<Sample> out;
    out.reserve(all.size());
    for (auto& s : all) {
        if (s.timestampNs >= sinceNs) out.push_back(s);
    }
    return out;
}

//  MetricsCollector 

MetricsCollector::MetricsCollector(const CollectorConfig& config)
    : _config(config)
{
    _registry.reserve(config.registryCapacity);

    if (config.enabled && config.alertDispatcher.enabled) {
        _alertDispatcher = std::make_unique<AlertDispatcher>(config.alertDispatcher);
        _alertDispatcher->start();
    }
}

MetricsCollector::~MetricsCollector()
{
    if (_alertDispatcher) _alertDispatcher->stop();
}

// Storage for the pre-configured instance config (set before first use).
static CollectorConfig& instanceConfigStore()
{
    static CollectorConfig cfg;  // default: enabled=true
    return cfg;
}

void MetricsCollector::setInstanceConfig(const CollectorConfig& config)
{
    // Only effective before the singleton is created.
    instanceConfigStore() = config;
}

MetricsCollector& MetricsCollector::instance()
{
    static MetricsCollector inst(instanceConfigStore());
    return inst;
}

std::string MetricsCollector::qualifiedName(const MetricDescriptor& desc) const
{
    if (_config.namePrefix.empty()) return desc.name;
    return _config.namePrefix + desc.name;
}

MetricsSnapshot MetricsCollector::snapshot() const
{
    MetricsSnapshot snap;
    snap.capturedAtNs = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    std::shared_lock<std::shared_mutex> lk(_registryMutex);

    for (const auto& [key, base] : _registry) {
        if (auto* c = dynamic_cast<const Counter*>(base.get())) {
            snap.counters.push_back({c->name(), c->value()});
        } else if (auto* g = dynamic_cast<const Gauge*>(base.get())) {
            snap.gauges.push_back({g->name(), g->value()});
        } else if (auto* s = dynamic_cast<const SlidingWindowSampler*>(base.get())) {
            SamplerSnapshot ss;
            ss.name          = s->name();
            ss.totalObserved = s->totalObserved();
            ss.droppedCount  = s->droppedCount();
            ss.mean          = s->mean();
            ss.samples       = s->allSamples();
            // Fixed quantile set used by all samplers comes from the collector default.
            for (double q : _config.defaultSampler.quantiles) {
                ss.quantiles.push_back(q);
                ss.quantileValues.push_back(s->percentile(q));
            }
            snap.samplers.push_back(std::move(ss));
        }
    }
    return snap;
}

} // namespace metrics
} // namespace va
