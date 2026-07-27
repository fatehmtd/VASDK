#pragma once

#include "Frame.h"
#include "metrics/MetricsCollector.h"
#include "metrics/MetricHandles.h"
#include "metrics/MetricsConfig.h"
#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

namespace va {
namespace pipeline {

/**
 * @brief Direction of frame flow in the pipeline
 */
enum class FlowDirection {
    DOWNSTREAM,  // Input → Output (normal flow)
    UPSTREAM     // Output → Input (interruptions, errors)
};

/**
 * @brief Base class for all frame processors in the pipeline.
 *
 * Receives frames, processes them, and forwards them to the next processor
 * in the chain. Supports bidirectional (downstream/upstream) flow and
 * priority-based frame handling.
 */
class VASDK_EXPORT FrameProcessor : public std::enable_shared_from_this<FrameProcessor> {
public:
    /**
     * @param collector  Collector to register metrics into. Null uses the global singleton.
     */
    explicit FrameProcessor(va::metrics::MetricsCollector* collector = nullptr);
    virtual ~FrameProcessor();

    /**
     * @brief Get processor name for debugging
     * @return Processor name
     */
    virtual std::string getName() const = 0;

    /**
     * @brief Initialize the processor
     * @return true if initialization succeeded
     */
    virtual bool initialize();

    /**
     * @brief Start processing frames
     * @return true if started successfully
     */
    virtual bool start();

    /**
     * @brief Stop processing frames
     */
    virtual void stop();

    /**
     * @brief Release resources
     */
    virtual void release();

    /**
     * @brief Check if processor is running
     * @return true if running
     */
    bool isRunning() const { return _running; }

    /**
     * @brief Configure base processor metrics (processing latency + queue wait samplers).
     */
    void setMetricsConfig(const va::metrics::ProcessorMetricsConfig& config);
    const va::metrics::ProcessorMetricsConfig& getMetricsConfig() const { return _baseMetrics.config; }

    /**
     * @brief Process a single frame
     * @param frame Frame to process
     * @param direction Flow direction
     */
    virtual void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) = 0;

    /**
     * @brief Chain this processor with the next downstream processor
     * @param next Next processor in the chain
     */
    void setDownstream(std::shared_ptr<FrameProcessor> next);

    /**
     * @brief Chain this processor with the previous upstream processor
     * @param prev Previous processor in the chain
     */
    void setUpstream(std::shared_ptr<FrameProcessor> prev);

    /**
     * @brief Get downstream processor
     * @return Downstream processor (may be null)
     */
    std::shared_ptr<FrameProcessor> getDownstream() const;

    /**
     * @brief Get upstream processor
     * @return Upstream processor (may be null)
     */
    std::shared_ptr<FrameProcessor> getUpstream() const;

protected:
    /**
     * @brief Push frame to downstream processor
     * @param frame Frame to push
     */
    void pushDownstream(std::shared_ptr<transport::Frame> frame);

    /**
     * @brief Push frame to upstream processor
     * @param frame Frame to push
     */
    void pushUpstream(std::shared_ptr<transport::Frame> frame);

    /**
     * @brief Enqueue frame for processing
     * @param frame Frame to enqueue
     * @param direction Flow direction
     */
    void enqueueFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction);

    /**
     * @brief Processing loop (runs in separate thread)
     */
    virtual void processingLoop();

    /**
     * @brief Check if this processor should handle this frame type
     * @param frame Frame to check
     * @return true if this processor should handle it
     */
    virtual bool shouldProcess(std::shared_ptr<transport::Frame> frame) const {
        return true; // By default, process all frames
    }

    // State
    std::atomic<bool> _running{false};
    std::atomic<bool> _initialized{false};

    // Collector used by derived initialize() to register handles. Null falls back to singleton.
    va::metrics::MetricsCollector* _collector{nullptr};

    struct BaseMetrics {
        va::metrics::ProcessorMetricsConfig config;
        va::metrics::SamplerHandle          proc;   // processing latency ms
        va::metrics::SamplerHandle          queue;  // queue wait ms
        va::metrics::CounterHandle          frames; // total frames processed
    } _baseMetrics;

private:
    // Frame queue with priority handling
    struct QueuedFrame {
        std::shared_ptr<transport::Frame> frame;
        FlowDirection direction;
        uint64_t insertionOrder{0};

        bool operator<(const QueuedFrame& other) const {
            // Higher priority frames should be processed first.
            // For equal priority, preserve FIFO insertion order.
            if (frame->getPriority() != other.frame->getPriority()) {
                return frame->getPriority() < other.frame->getPriority();
            }
            return insertionOrder > other.insertionOrder;
        }
    };

    std::priority_queue<QueuedFrame> _frameQueue;
    uint64_t _insertionCounter{0};
    std::queue<QueuedFrame> _systemFrameQueue; // Separate queue for system frames
    std::mutex _queueMutex;
    std::condition_variable _queueCondVar;

    // Processing thread
    std::thread _processingThread;

    // Chain links
    std::weak_ptr<FrameProcessor> _downstream;
    std::weak_ptr<FrameProcessor> _upstream;
    mutable std::mutex _chainMutex;
};

/**
 * @brief Pass-through processor that forwards all frames unchanged
 *
 * Useful for debugging, logging, or as a base for simple processors
 */
class VASDK_EXPORT PassThroughProcessor : public FrameProcessor {
public:
    std::string getName() const override { return "PassThroughProcessor"; }

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override {
        // Simply forward the frame
        if (direction == FlowDirection::DOWNSTREAM) {
            pushDownstream(frame);
        } else {
            pushUpstream(frame);
        }
    }
};

/**
 * @brief Processor that logs all frames passing through
 */
class VASDK_EXPORT LoggingProcessor : public FrameProcessor {
public:
    std::string getName() const override { return "LoggingProcessor"; }

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

private:
    std::string frameTypeToString(transport::FrameType type) const;
    std::string directionToString(FlowDirection direction) const;
};

} // namespace pipeline
} // namespace va
