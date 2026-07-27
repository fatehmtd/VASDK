#pragma once

#include "MetricsCollector.h"

namespace va {
namespace metrics {

// Thin typed wrappers around nullable metric pointers.
// They remove call-site boilerplate while keeping disabled-metrics behavior safe.
class CounterHandle {
public:
    CounterHandle() = default;

    void initialize(MetricsCollector* collector, const MetricDescriptor& descriptor) {
        _counter = (collector != nullptr) ? collector->get<Counter>(descriptor) : nullptr;
    }

    void increment() noexcept {
        if (_counter) {
            _counter->increment();
        }
    }

    void add(uint64_t n) noexcept {
        if (_counter) {
            _counter->add(n);
        }
    }

    Counter* get() const noexcept { return _counter; }
    explicit operator bool() const noexcept { return _counter != nullptr; }

private:
    Counter* _counter{nullptr};
};

class SamplerHandle {
public:
    SamplerHandle() = default;

    void initialize(MetricsCollector* collector, const MetricDescriptor& descriptor) {
        _sampler = (collector != nullptr) ? collector->get<SlidingWindowSampler>(descriptor) : nullptr;
    }

    void observe(double value) noexcept {
        if (_sampler) {
            _sampler->observe(value);
        }
    }

    SlidingWindowSampler* get() const noexcept { return _sampler; }
    explicit operator bool() const noexcept { return _sampler != nullptr; }

private:
    SlidingWindowSampler* _sampler{nullptr};
};

class GaugeHandle {
public:
    GaugeHandle() = default;

    void initialize(MetricsCollector* collector, const MetricDescriptor& descriptor) {
        _gauge = (collector != nullptr) ? collector->get<Gauge>(descriptor) : nullptr;
    }

    void set(double value) noexcept {
        if (_gauge) {
            _gauge->set(value);
        }
    }

    void setThreshold(double threshold, AlertDispatcher* dispatcher) {
        if (_gauge) {
            _gauge->setThreshold(threshold, dispatcher);
        }
    }

    Gauge* get() const noexcept { return _gauge; }
    explicit operator bool() const noexcept { return _gauge != nullptr; }

private:
    Gauge* _gauge{nullptr};
};

class ScopeCounterIncrement {
public:
    explicit ScopeCounterIncrement(CounterHandle& counter) noexcept
        : _counter(counter) {}

    ~ScopeCounterIncrement() {
        _counter.increment();
    }

    ScopeCounterIncrement(const ScopeCounterIncrement&) = delete;
    ScopeCounterIncrement& operator=(const ScopeCounterIncrement&) = delete;

private:
    CounterHandle& _counter;
};

namespace timing_stages {
static constexpr const char* kEncoder = "encoder";
static constexpr const char* kDecoder = "decoder";
}  // namespace timing_stages

}  // namespace metrics
}  // namespace va
