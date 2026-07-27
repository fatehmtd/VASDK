#pragma once

#include "Frame.h"
#include "metrics/MetricsCollector.h"
#include "metrics/MetricHandles.h"
#include "metrics/MetricsConfig.h"
#include <functional>
#include <memory>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace va {
namespace transport {

/**
 * @brief Transport state
 */
enum class TransportState {
    DISCONNECTED,  // Not connected
    CONNECTING,    // Connection in progress
    CONNECTED,     // Connected and ready
    DISCONNECTING, // Disconnection in progress
    ERROR          // Error state
};

/**
 * @brief Transport statistics
 */
struct TransportStats {
    uint64_t framesReceived = 0;
    uint64_t framesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t bytesSent = 0;
    uint64_t errors = 0;
    uint64_t droppedFrames = 0;
};

/**
 * @brief Base transport interface
 *
 * Transports handle the "how" of getting audio/video/data in and out
 * of the voice assistant pipeline. They abstract the connection layer
 * (local hardware, WebSocket, WebRTC, telephony, etc.) from the
 * processing logic.
 *
 * This enables the same pipeline to work with different input/output
 * mechanisms without any code changes.
 */
class VASDK_EXPORT Transport : public std::enable_shared_from_this<Transport> {
public:
    virtual ~Transport() = default;

    /**
     * @brief Get transport name/type
     * @return Transport name (e.g., "LocalTransport", "WebSocketTransport")
     */
    virtual std::string getName() const = 0;

    /**
     * @brief Get transport description
     * @return Human-readable description
     */
    virtual std::string getDescription() const = 0;

    // ========== Lifecycle Methods ==========

    /**
     * @brief Initialize the transport
     * @return true if initialization succeeded
     */
    virtual bool initialize() = 0;

    /**
     * @brief Start the transport (begin receiving/sending frames)
     * @return true if started successfully
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the transport
     */
    virtual void stop() = 0;

    /**
     * @brief Release resources
     */
    virtual void release() = 0;

    // ========== Connection Status ==========

    /**
     * @brief Check if transport is connected
     * @return true if connected
     */
    virtual bool isConnected() const = 0;

    /**
     * @brief Get current transport state
     * @return TransportState
     */
    virtual TransportState getState() const = 0;

    // ========== Frame I/O ==========

    /**
     * @brief Receive a frame from the transport (blocking)
     * @param timeoutMs Timeout in milliseconds (0 = no wait, -1 = infinite)
     * @return Received frame or nullptr if timeout/error
     */
    virtual std::shared_ptr<Frame> receiveFrame(int timeoutMs = -1) = 0;

    /**
     * @brief Send a frame through the transport
     * @param frame Frame to send
     * @return true if sent successfully
     */
    virtual bool sendFrame(std::shared_ptr<Frame> frame) = 0;

    /**
     * @brief Check if frames are available to receive
     * @return true if frames are available
     */
    virtual bool hasFrames() const = 0;

    // ========== Event Callbacks ==========

    /**
     * @brief Callback fired when transport connects
     */
    std::function<void()> onConnected;

    /**
     * @brief Callback fired when transport disconnects
     */
    std::function<void()> onDisconnected;

    /**
     * @brief Callback fired when an error occurs
     * @param errorCode Error code
     * @param errorMessage Error message
     */
    std::function<void(int errorCode, const std::string& errorMessage)> onError;

    /**
     * @brief Callback fired when a frame is received (optional, non-blocking alternative)
     * @param frame Received frame
     */
    std::function<void(std::shared_ptr<Frame> frame)> onFrameReceived;

    // ========== Statistics ==========

    /**
     * @brief Get transport statistics
     * @return TransportStats
     */
    virtual TransportStats getStats() const = 0;

    /**
     * @brief Reset statistics
     */
    virtual void resetStats() = 0;

protected:
    Transport() = default;

    /**
     * @brief Fire the onConnected callback safely
     */
    void fireOnConnected() {
        if (onConnected) {
            onConnected();
        }
    }

    /**
     * @brief Fire the onDisconnected callback safely
     */
    void fireOnDisconnected() {
        if (onDisconnected) {
            onDisconnected();
        }
    }

    /**
     * @brief Fire the onError callback safely
     */
    void fireOnError(int errorCode, const std::string& errorMessage) {
        if (onError) {
            onError(errorCode, errorMessage);
        }
    }

    /**
     * @brief Fire the onFrameReceived callback safely
     */
    void fireOnFrameReceived(std::shared_ptr<Frame> frame) {
        if (onFrameReceived) {
            onFrameReceived(frame);
        }
    }
};

/**
 * @brief Base transport implementation with frame queue
 *
 * Provides common functionality for transports:
 * - Thread-safe frame queue
 * - Statistics tracking
 * - State management
 */
class VASDK_EXPORT BaseTransport : public Transport {
public:
    BaseTransport();
    virtual ~BaseTransport();

    // Implement common methods
    bool isConnected() const override;
    TransportState getState() const override;
    bool hasFrames() const override;
    std::shared_ptr<Frame> receiveFrame(int timeoutMs = -1) override;
    TransportStats getStats() const override;
    void resetStats() override;

    void setMetrics(va::metrics::MetricsCollector* metrics);
    void setMetricsConfig(const va::metrics::TransportMetricsConfig& config);
    const va::metrics::TransportMetricsConfig& getTransportMetricsConfig() const { return _metricsConfig; }

protected:
    /**
     * @brief Enqueue a received frame (called by transport implementations)
     * @param frame Frame to enqueue
     */
    void enqueueReceivedFrame(std::shared_ptr<Frame> frame);

    /**
     * @brief Update transport state
     * @param newState New state
     */
    void setState(TransportState newState);

    /**
     * @brief Update statistics for sent frame
     * @param frameSize Size in bytes
     */
    void updateSentStats(size_t frameSize);

    /**
     * @brief Update statistics for received frame
     * @param frameSize Size in bytes
     */
    void updateReceivedStats(size_t frameSize);

    /**
     * @brief Increment error count
     */
    void incrementErrorCount();

    /**
     * @brief Increment dropped frame count
     */
    void incrementDroppedFrameCount();

    // State
    TransportState _state;
    mutable std::mutex _stateMutex;

    // Frame queue (for received frames)
    std::queue<std::shared_ptr<Frame>> _receiveQueue;
    mutable std::mutex _queueMutex;
    std::condition_variable _queueCondVar;
    size_t _maxQueueSize;

    // Statistics
    TransportStats _stats;
    mutable std::mutex _statsMutex;

    // Metrics
    va::metrics::MetricsCollector*       _collector{nullptr};
    va::metrics::TransportMetricsConfig  _metricsConfig;
    void initializeMetrics();

    va::metrics::CounterHandle _txFramesMetric;
    va::metrics::CounterHandle _rxFramesMetric;
    va::metrics::CounterHandle _txBytesMetric;
    va::metrics::CounterHandle _rxBytesMetric;
    va::metrics::CounterHandle _errorMetric;
    va::metrics::CounterHandle _droppedMetric;
    va::metrics::GaugeHandle   _connectedGauge;
};

} // namespace transport
} // namespace va
