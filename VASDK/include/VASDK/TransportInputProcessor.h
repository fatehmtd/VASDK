#pragma once

#include "FrameProcessor.h"
#include "Transport.h"
#include <memory>
#include <vector>

namespace va {
namespace pipeline {

/**
 * @brief Fan-in input processor: pulls frames from one or more transports into the pipeline.
 *
 * Each transport's onFrameReceived callback is wired to enqueue frames directly into
 * this processor's queue - no polling thread. The transport's own I/O thread fires
 * the callback; this processor's standard thread dequeues and calls pushDownstream().
 *
 * Call addTransport() for each input transport before initialize().
 */
class VASDK_EXPORT TransportInputProcessor : public FrameProcessor {
public:
    TransportInputProcessor() = default;
    ~TransportInputProcessor() override;

    /**
     * @brief Add an input transport (fan-in source).
     * Must be called before initialize().
     */
    void addTransport(std::shared_ptr<transport::Transport> transport);

    std::string getName() const override { return "TransportInputProcessor"; }

    bool initialize() override;
    bool start() override;
    void stop() override;

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

private:
    std::vector<std::shared_ptr<transport::Transport>> _transports;
};

} // namespace pipeline
} // namespace va
