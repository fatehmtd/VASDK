#include "VASDK/TransportOutputProcessor.h"
#include <spdlog/spdlog.h>

namespace va {
namespace pipeline {

TransportOutputProcessor::~TransportOutputProcessor() {
    stop();
}

void TransportOutputProcessor::addTransport(std::shared_ptr<transport::Transport> transport,
                                            std::optional<transport::FrameType> filter) {
    if (transport) _outputs.push_back({std::move(transport), filter});
}

bool TransportOutputProcessor::initialize() {
    if (!FrameProcessor::initialize()) return false;

    if (_outputs.empty()) {
        spdlog::error("[TransportOutputProcessor] No transports registered");
        return false;
    }

    for (auto& out : _outputs) {
        if (!out.transport->isConnected() &&
            out.transport->getState() == transport::TransportState::DISCONNECTED) {
            if (!out.transport->initialize()) {
                spdlog::error("[TransportOutputProcessor] Failed to initialize transport: {}",
                              out.transport->getName());
                return false;
            }
        }
        spdlog::info("[TransportOutputProcessor] Registered transport: {} (filter: {})",
                     out.transport->getName(),
                     out.filter ? std::to_string(static_cast<int>(*out.filter)) : "all");
    }
    return true;
}

bool TransportOutputProcessor::start() {
    if (!FrameProcessor::start()) return false;

    for (auto& out : _outputs) {
        if (!out.transport->isConnected()) {
            if (!out.transport->start()) {
                spdlog::error("[TransportOutputProcessor] Failed to start transport: {}",
                              out.transport->getName());
                return false;
            }
        }
    }
    spdlog::info("[TransportOutputProcessor] Started ({} transports)", _outputs.size());
    return true;
}

void TransportOutputProcessor::stop() {
    for (auto& out : _outputs) {
        out.transport->stop();
    }
    FrameProcessor::stop();
    spdlog::info("[TransportOutputProcessor] Stopped");
}

void TransportOutputProcessor::processFrame(std::shared_ptr<transport::Frame> frame,
                                            FlowDirection direction) {
    if (direction == FlowDirection::DOWNSTREAM) {
        for (auto& out : _outputs) {
            if (!out.filter || frame->getType() == *out.filter) {
                if (!out.transport->sendFrame(frame)) {
                    spdlog::warn("[TransportOutputProcessor] Failed to send frame type {} via {}",
                                 static_cast<int>(frame->getType()),
                                 out.transport->getName());
                }
            }
        }
    }
    // Always forward - downstream processors still see all frames
    pushDownstream(frame);
}

} // namespace pipeline
} // namespace va
