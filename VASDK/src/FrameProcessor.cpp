#include "VASDK/FrameProcessor.h"
#include <spdlog/spdlog.h>

namespace va {
namespace pipeline {

FrameProcessor::FrameProcessor(va::metrics::MetricsCollector* collector)
    : _running(false)
    , _initialized(false)
    , _collector(collector ? collector : &va::metrics::MetricsCollector::instance())
{
}

FrameProcessor::~FrameProcessor() {
    // Do NOT call stop() here: it calls getName(), a pure virtual, but the derived
    // vtable is already gone by the time this base dtor runs. Join the thread inline instead.
    _running = false;
    _queueCondVar.notify_all();
    if (_processingThread.joinable()) {
        _processingThread.join();
    }
}

bool FrameProcessor::initialize() {
    if (_initialized.exchange(true)) {
        spdlog::warn("[{}] Already initialized", getName());
        return true;
    }

    // Pre-register base processor metrics if enabled.
    if (_baseMetrics.config.enabled && _collector) {
        // Include processor name in the metric key so multiple processors don't collide.
        std::string procKey  = getName() + "_processing_ms";
        std::string queueKey = getName() + "_queue_wait_ms";
        std::string ctKey    = getName() + "_frames_total";

        va::metrics::MetricDescriptor pd{procKey.c_str(),
            va::metrics::MetricType::Sampler, "Processing latency ms",
            _baseMetrics.config.processingWindow};
        va::metrics::MetricDescriptor qd{queueKey.c_str(),
            va::metrics::MetricType::Sampler, "Queue wait ms",
            _baseMetrics.config.queueWindow};
        va::metrics::MetricDescriptor cd{ctKey.c_str(),
            va::metrics::MetricType::Counter, "Total frames processed"};

        _baseMetrics.proc.initialize(_collector, pd);
        _baseMetrics.queue.initialize(_collector, qd);
        _baseMetrics.frames.initialize(_collector, cd);
    }

    spdlog::info("[{}] Initialized", getName());
    return true;
}

bool FrameProcessor::start() {
    if (_running) {
        spdlog::warn("[{}] Already running", getName());
        return true;
    }

    if (!_initialized) {
        spdlog::error("[{}] Not initialized", getName());
        return false;
    }

    _running = true;
    _processingThread = std::thread(&FrameProcessor::processingLoop, this);
    spdlog::info("[{}] Started", getName());
    return true;
}

void FrameProcessor::stop() {
    if (!_running) {
        return;
    }

    _running = false;
    _queueCondVar.notify_all();

    if (_processingThread.joinable()) {
        _processingThread.join();
    }

    spdlog::info("[{}] Stopped", getName());
}

void FrameProcessor::release() {
    stop();
    _initialized = false;
    spdlog::info("[{}] Released", getName());
}

void FrameProcessor::setMetricsConfig(const va::metrics::ProcessorMetricsConfig& config)
{
    _baseMetrics.config = config;
}

void FrameProcessor::setDownstream(std::shared_ptr<FrameProcessor> next) {
    std::lock_guard<std::mutex> lock(_chainMutex);
    _downstream = next;
    if (next) {
        next->_upstream = shared_from_this();
    }
}

void FrameProcessor::setUpstream(std::shared_ptr<FrameProcessor> prev) {
    std::lock_guard<std::mutex> lock(_chainMutex);
    _upstream = prev;
}

std::shared_ptr<FrameProcessor> FrameProcessor::getDownstream() const {
    std::lock_guard<std::mutex> lock(_chainMutex);
    return _downstream.lock();
}

std::shared_ptr<FrameProcessor> FrameProcessor::getUpstream() const {
    std::lock_guard<std::mutex> lock(_chainMutex);
    return _upstream.lock();
}

void FrameProcessor::pushDownstream(std::shared_ptr<transport::Frame> frame) {
    auto next = getDownstream();
    if (next) {
        next->enqueueFrame(frame, FlowDirection::DOWNSTREAM);
    }
}

void FrameProcessor::pushUpstream(std::shared_ptr<transport::Frame> frame) {
    auto prev = getUpstream();
    if (prev) {
        prev->enqueueFrame(frame, FlowDirection::UPSTREAM);
    }
}

void FrameProcessor::enqueueFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
    std::lock_guard<std::mutex> lock(_queueMutex);

    QueuedFrame qf{frame, direction, _insertionCounter++};

    // System frames go to priority queue
    if (frame->getPriority() == transport::FramePriority::CRITICAL) {
        _systemFrameQueue.push(qf);
    } else {
        _frameQueue.push(qf);
    }

    _queueCondVar.notify_one();
}

void FrameProcessor::processingLoop() {
    spdlog::debug("[{}] Processing loop started", getName());

    while (_running) {
        QueuedFrame qf;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueCondVar.wait(lock, [this] {
                return !_systemFrameQueue.empty() || !_frameQueue.empty() || !_running;
            });

            if (!_running) {
                break;
            }

            // Process system frames first
            if (!_systemFrameQueue.empty()) {
                qf = _systemFrameQueue.front();
                _systemFrameQueue.pop();
            } else if (!_frameQueue.empty()) {
                qf = _frameQueue.top();
                _frameQueue.pop();
            } else {
                continue;
            }
        }

        // Process frame outside the lock
        try {
            if (qf.frame) {
                // Acquisition = time the frame spent waiting in the queue (microseconds -> ms)
                qf.frame->acquisitionDuration =
                    (transport::Frame::getCurrentTimestamp() - qf.frame->timestamp) / 1000;
            }
            if (shouldProcess(qf.frame)) {
                auto processingStartTime = std::chrono::high_resolution_clock::now();
                processFrame(qf.frame, qf.direction);
                auto processingEndTime = std::chrono::high_resolution_clock::now();
                auto processingDuration = std::chrono::duration_cast<std::chrono::milliseconds>(processingEndTime - processingStartTime).count();
                qf.frame->processingDuration = processingDuration;

                // Lock-free metric observations - direct handle calls, no registry lookup.
                _baseMetrics.proc.observe(static_cast<double>(processingDuration));
                _baseMetrics.queue.observe(static_cast<double>(qf.frame->acquisitionDuration));
                _baseMetrics.frames.increment();

                spdlog::debug("[{}] Processed frame: type={}, id={}, seq={}, timestamp={}ms, acquisition={}ms, processing={}ms",
                    getName(),
                    (int)qf.frame->getType(),
                    qf.frame->frameId,
                    qf.frame->sequenceId,
                    qf.frame->timestamp,
                    qf.frame->acquisitionDuration,
                    qf.frame->processingDuration);
            } else {
                // Forward frame without processing
                if (qf.direction == FlowDirection::DOWNSTREAM) {
                    pushDownstream(qf.frame);
                } else {
                    pushUpstream(qf.frame);
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[{}] Error processing frame: {}", getName(), e.what());
        }
    }

    spdlog::debug("[{}] Processing loop stopped", getName());
}

// LoggingProcessor implementation
void LoggingProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
    spdlog::info("[LoggingProcessor] {} frame: type={}, id={}, seq={}, timestamp={}",
        directionToString(direction),
        frameTypeToString(frame->getType()),
        frame->frameId,
        frame->sequenceId,
        frame->timestamp);

    if (direction == FlowDirection::DOWNSTREAM) {
        pushDownstream(frame);
    } else {
        pushUpstream(frame);
    }
}

std::string LoggingProcessor::frameTypeToString(transport::FrameType type) const {
    return transport::toString(type);
}

std::string LoggingProcessor::directionToString(FlowDirection direction) const {
    return (direction == FlowDirection::DOWNSTREAM) ? "DOWNSTREAM" : "UPSTREAM";
}

} // namespace pipeline
} // namespace va
