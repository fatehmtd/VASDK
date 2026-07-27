#include "VASDK/TransportInputProcessor.h"
#include <spdlog/spdlog.h>

namespace va {
namespace pipeline {

TransportInputProcessor::~TransportInputProcessor() {
    stop();
}

void TransportInputProcessor::addTransport(std::shared_ptr<transport::Transport> transport) {
    if (transport) _transports.push_back(std::move(transport));
}

bool TransportInputProcessor::initialize() {
    if (!FrameProcessor::initialize()) return false;

    if (_transports.empty()) {
        spdlog::error("[TransportInputProcessor] No transports registered");
        return false;
    }

    for (auto& t : _transports) {
        if (!t->isConnected() &&
            t->getState() == transport::TransportState::DISCONNECTED) {
            if (!t->initialize()) {
                spdlog::error("[TransportInputProcessor] Failed to initialize transport: {}",
                              t->getName());
                return false;
            }
        }
        // Wire push-based callback: transport I/O thread → this processor's queue
        t->onFrameReceived = [this](std::shared_ptr<transport::Frame> frame) {
            enqueueFrame(frame, FlowDirection::DOWNSTREAM);
        };
        spdlog::info("[TransportInputProcessor] Registered transport: {}", t->getName());
    }
    return true;
}

bool TransportInputProcessor::start() {
    if (!FrameProcessor::start()) return false;

    for (auto& t : _transports) {
        if (!t->isConnected()) {
            if (!t->start()) {
                spdlog::error("[TransportInputProcessor] Failed to start transport: {}",
                              t->getName());
                return false;
            }
        }
    }
    spdlog::info("[TransportInputProcessor] Started ({} transports)", _transports.size());
    return true;
}

void TransportInputProcessor::stop() {
    for (auto& t : _transports) {
        t->onFrameReceived = nullptr;
        t->stop();
    }
    FrameProcessor::stop();
    spdlog::info("[TransportInputProcessor] Stopped");
}

void TransportInputProcessor::processFrame(std::shared_ptr<transport::Frame> frame,
                                           FlowDirection direction) {
    if (direction == FlowDirection::DOWNSTREAM) {
        pushDownstream(frame);
    }
}

} // namespace pipeline
} // namespace va
