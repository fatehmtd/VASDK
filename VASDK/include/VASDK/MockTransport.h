#pragma once

#include "Transport.h"
#include <vector>

namespace va {
namespace transport {

/**
 * @brief Mock transport for testing - injects frames into the pipeline
 * without requiring real audio hardware or network connections.
 */
class VASDK_EXPORT MockTransport : public BaseTransport {
public:
    MockTransport();
    virtual ~MockTransport();

    // Transport interface
    std::string getName() const override { return "MockTransport"; }
    std::string getDescription() const override {
        return "Mock transport for testing (no real I/O)";
    }

    bool initialize() override;
    bool start() override;
    void stop() override;
    void release() override;
    bool sendFrame(std::shared_ptr<Frame> frame) override;

    /**
     * @brief Inject a frame into the receive queue (simulates receiving)
     * @param frame Frame to inject
     */
    void injectFrame(std::shared_ptr<Frame> frame);

    /**
     * @brief Get all sent frames
     * @return Vector of sent frames
     */
    std::vector<std::shared_ptr<Frame>> getSentFrames() const;

    /**
     * @brief Get sent frames of a specific type
     * @param type Frame type to filter
     * @return Vector of sent frames matching type
     */
    std::vector<std::shared_ptr<Frame>> getSentFramesByType(FrameType type) const;

    /**
     * @brief Clear all sent frames
     */
    void clearSentFrames();

    /**
     * @brief Get count of sent frames
     * @return Number of frames sent
     */
    size_t getSentFrameCount() const;

    /**
     * @brief Set automatic response mode
     *
     * When enabled, the mock transport automatically responds to
     * certain frame types with predefined responses.
     *
     * @param enabled True to enable auto-response
     */
    void setAutoResponse(bool enabled) {
        _autoResponse = enabled;
    }

    /**
     * @brief Set simulated latency (in milliseconds)
     * @param latencyMs Latency to simulate (0 = no latency)
     */
    void setSimulatedLatency(int latencyMs) {
        _simulatedLatencyMs = latencyMs;
    }

    /**
     * @brief Simulate an error
     * @param errorCode Error code
     * @param errorMessage Error message
     */
    void simulateError(int errorCode, const std::string& errorMessage);

    /**
     * @brief Simulate disconnection
     */
    void simulateDisconnection();

private:
    /**
     * @brief Auto-respond to certain frame types
     */
    void handleAutoResponse(std::shared_ptr<Frame> frame);

    // Sent frames (for verification in tests)
    std::vector<std::shared_ptr<Frame>> _sentFrames;
    mutable std::mutex _sentFramesMutex;

    // Mock configuration
    bool _autoResponse;
    int _simulatedLatencyMs;
};

} // namespace transport
} // namespace va
