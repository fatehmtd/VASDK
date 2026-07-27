#pragma once

#include "FrameProcessor.h"
#include "Transport.h"
#include <memory>
#include <optional>
#include <vector>

namespace va {
namespace pipeline {

/**
 * @brief Fan-out output processor: sends frames to one or more transports from the pipeline.
 *
 * Each output transport has an optional FrameType filter. If the filter is set, only frames
 * of that type are forwarded to that transport. If no filter is set, all frames are forwarded.
 * Frames are always pushed downstream after the fan-out so the rest of the chain still sees them.
 *
 * Call addTransport() for each output transport before initialize().
 */
class VASDK_EXPORT TransportOutputProcessor : public FrameProcessor {
public:
    TransportOutputProcessor() = default;
    ~TransportOutputProcessor() override;

    /**
     * @brief Add an output transport (fan-out destination).
     * @param transport  Transport to send frames to.
     * @param filter     Only forward frames of this type; nullopt = all frames.
     * Must be called before initialize().
     */
    void addTransport(std::shared_ptr<transport::Transport> transport,
                      std::optional<transport::FrameType> filter = std::nullopt);

    std::string getName() const override { return "TransportOutputProcessor"; }

    bool initialize() override;
    bool start() override;
    void stop() override;

    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

private:
    struct Output {
        std::shared_ptr<transport::Transport> transport;
        std::optional<transport::FrameType>   filter;
    };
    std::vector<Output> _outputs;
};

} // namespace pipeline
} // namespace va
