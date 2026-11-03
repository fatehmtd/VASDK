#pragma once

#include "FrameProcessor.h"
#include "WakeWordProvider.h"

#include <functional>
#include <memory>
#include <vector>

namespace va {

class AudioRingBuffer;

namespace pipeline {

/**
 * @brief Runs a WakeWordProvider on the mic audio and sends TriggerFrame{WAKE_WORD} on a detection,
 *        which wakes an IDLE AgentStateProcessor. Place it before the VAD.
 *
 * Only listens while the agent is IDLE: once it is awake, the phrase means nothing.
 */
class VASDK_EXPORT WakeWordProcessor : public FrameProcessor {
public:
    explicit WakeWordProcessor(std::shared_ptr<audio::WakeWordProvider> provider) : _provider(std::move(provider)) {}
    std::string getName() const override { return "WakeWordProcessor"; }
    std::string getProviderName() const override { return _provider ? _provider->getName() : ""; }
    void setRingBuffer(std::shared_ptr<AudioRingBuffer> ringBuffer) { _ringBuffer = std::move(ringBuffer); }
    /** Whether the agent is IDLE, e.g. AgentStateProcessor::getState(); it sits after this stage. Unset: always. */
    void setAgentIdle(std::function<bool()> agentIdle) { _agentIdle = std::move(agentIdle); }

    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

private:
    std::shared_ptr<audio::WakeWordProvider> _provider;
    std::shared_ptr<AudioRingBuffer> _ringBuffer;
    std::vector<char> _samples;
    std::function<bool()> _agentIdle;
    bool _wasIdle = true;
};

} // namespace pipeline
} // namespace va
