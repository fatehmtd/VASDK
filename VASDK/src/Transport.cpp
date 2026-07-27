#include "VASDK/Transport.h"

namespace va {
namespace transport {

BaseTransport::BaseTransport()
    : _state(TransportState::DISCONNECTED)
    , _maxQueueSize(1000) // Default max queue size
{
    _collector = &va::metrics::MetricsCollector::instance();
}

BaseTransport::~BaseTransport() {
}

bool BaseTransport::isConnected() const {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _state == TransportState::CONNECTED;
}

TransportState BaseTransport::getState() const {
    std::lock_guard<std::mutex> lock(_stateMutex);
    return _state;
}

bool BaseTransport::hasFrames() const {
    std::lock_guard<std::mutex> lock(_queueMutex);
    return !_receiveQueue.empty();
}

std::shared_ptr<Frame> BaseTransport::receiveFrame(int timeoutMs) {
    std::unique_lock<std::mutex> lock(_queueMutex);

    if (timeoutMs == 0) {
        // Non-blocking: return immediately
        if (_receiveQueue.empty()) {
            return nullptr;
        }
        auto frame = _receiveQueue.front();
        _receiveQueue.pop();
        return frame;
    } else if (timeoutMs < 0) {
        // Blocking: wait indefinitely
        _queueCondVar.wait(lock, [this] {
            return !_receiveQueue.empty();
        });
        auto frame = _receiveQueue.front();
        _receiveQueue.pop();
        return frame;
    } else {
        // Blocking with timeout
        auto timeout = std::chrono::milliseconds(timeoutMs);
        if (_queueCondVar.wait_for(lock, timeout, [this] {
                return !_receiveQueue.empty();
            })) {
            auto frame = _receiveQueue.front();
            _receiveQueue.pop();
            return frame;
        }
        return nullptr; // Timeout
    }
}

TransportStats BaseTransport::getStats() const {
    std::lock_guard<std::mutex> lock(_statsMutex);
    return _stats;
}

void BaseTransport::resetStats() {
    std::lock_guard<std::mutex> lock(_statsMutex);
    _stats = TransportStats();
}

void BaseTransport::enqueueReceivedFrame(std::shared_ptr<Frame> frame) {
    {
        std::lock_guard<std::mutex> lock(_queueMutex);

        if (_receiveQueue.size() >= _maxQueueSize) {
            _receiveQueue.pop(); // drop oldest
            incrementDroppedFrameCount();
        }

        _receiveQueue.push(frame);
    }

    _queueCondVar.notify_one();
    fireOnFrameReceived(frame);
}

void BaseTransport::setState(TransportState newState) {
    TransportState oldState;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        oldState = _state;
        _state = newState;
    }

    // Fire callbacks on state transitions
    if (oldState != newState) {
        if (newState == TransportState::CONNECTED) {
            _connectedGauge.set(1.0);
            fireOnConnected();
        } else if (newState == TransportState::DISCONNECTED && oldState == TransportState::CONNECTED) {
            _connectedGauge.set(0.0);
            fireOnDisconnected();
        }
    }
}

void BaseTransport::updateSentStats(size_t frameSize) {
    {
        std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.framesSent++;
        _stats.bytesSent += frameSize;
    }
    _txFramesMetric.increment();
    _txBytesMetric.add(static_cast<uint64_t>(frameSize));
}

void BaseTransport::updateReceivedStats(size_t frameSize) {
    {
        std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.framesReceived++;
        _stats.bytesReceived += frameSize;
    }
    _rxFramesMetric.increment();
    _rxBytesMetric.add(static_cast<uint64_t>(frameSize));
}

void BaseTransport::incrementErrorCount() {
    {
        std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.errors++;
    }
    _errorMetric.increment();
}

void BaseTransport::incrementDroppedFrameCount() {
    {
        std::lock_guard<std::mutex> lock(_statsMutex);
        _stats.droppedFrames++;
    }
    _droppedMetric.increment();
}

void BaseTransport::setMetrics(va::metrics::MetricsCollector* metrics)
{
    _collector = metrics ? metrics : &va::metrics::MetricsCollector::instance();
}

void BaseTransport::setMetricsConfig(const va::metrics::TransportMetricsConfig& config)
{
    _metricsConfig = config;
    initializeMetrics();
}

void BaseTransport::initializeMetrics()
{
    if (!_metricsConfig.enabled || !_collector) return;
    using namespace va::metrics;
    const std::string prefix = _metricsConfig.namePrefix.empty() ? getName() : _metricsConfig.namePrefix;
    // MetricDescriptor::name is const char* - store each name string first so .c_str() stays valid.
    const std::string nTxFrames  = prefix + "_tx_frames_total";
    const std::string nRxFrames  = prefix + "_rx_frames_total";
    const std::string nTxBytes   = prefix + "_tx_bytes_total";
    const std::string nRxBytes   = prefix + "_rx_bytes_total";
    const std::string nErrors    = prefix + "_errors_total";
    const std::string nDropped   = prefix + "_dropped_total";
    const std::string nConnected = prefix + "_connected";
    _txFramesMetric.initialize(_collector, MetricDescriptor{nTxFrames.c_str(),  MetricType::Counter, "Frames sent"});
    _rxFramesMetric.initialize(_collector, MetricDescriptor{nRxFrames.c_str(),  MetricType::Counter, "Frames received"});
    _txBytesMetric.initialize(_collector, MetricDescriptor{nTxBytes.c_str(),   MetricType::Counter, "Bytes sent"});
    _rxBytesMetric.initialize(_collector, MetricDescriptor{nRxBytes.c_str(),   MetricType::Counter, "Bytes received"});
    _errorMetric.initialize(_collector, MetricDescriptor{nErrors.c_str(),    MetricType::Counter, "Transport errors"});
    _droppedMetric.initialize(_collector, MetricDescriptor{nDropped.c_str(),   MetricType::Counter, "Dropped frames"});
    _connectedGauge.initialize(_collector, MetricDescriptor{nConnected.c_str(), MetricType::Gauge,   "1 if connected, 0 otherwise"});
}

} // namespace transport
} // namespace va
