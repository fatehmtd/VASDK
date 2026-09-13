#include <VASDK/orchestrator/OrchestratorFrameProcessor.h>

#include <VASDK/Frame.h>

namespace va {
namespace orchestrator {

OrchestratorFrameProcessor::OrchestratorFrameProcessor(std::shared_ptr<OrchestratorEngine> engine)
    : _engine(std::move(engine)) {
    _engine->setOnSay([this](const std::string& text) {
        auto frame = std::make_shared<va::transport::TextFrame>();
        frame->text = text;
        frame->isFinal = true;
        pushDownstream(frame);
    });
}

void OrchestratorFrameProcessor::processFrame(std::shared_ptr<va::transport::Frame> frame,
                                              va::pipeline::FlowDirection direction) {
    if (direction != va::pipeline::FlowDirection::DOWNSTREAM) {
        // M1.6 has no in-flight external call to cancel on an upstream
        // ControlFrame::INTERRUPT yet (callWebhook/llmGenerate are Phase 2/3,
        // spec §9.3) - nothing to do.
        return;
    }

    switch (frame->getType()) {
    case va::transport::FrameType::TURN_STATE: {
        auto turnState = std::static_pointer_cast<va::transport::TurnStateFrame>(frame);
        if (turnState->state == va::transport::TurnStateFrame::TurnState::START) {
            _engine->start();
        }
        break;
    }
    case va::transport::FrameType::TRANSCRIPTION: {
        auto transcription = std::static_pointer_cast<va::transport::TranscriptionFrame>(frame);
        if (transcription->isFinal) {
            _engine->handleUtterance(transcription->transcriptionResult.text);
        }
        break;
    }
    case va::transport::FrameType::CONTROL: {
        // ControlFrame::INTERRUPT - see the note above; every other control
        // command is outside this processor's concern.
        break;
    }
    default:
        break;
    }
}

} // namespace orchestrator
} // namespace va
