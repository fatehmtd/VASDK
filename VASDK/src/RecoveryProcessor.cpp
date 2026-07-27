#include <VASDK/RecoveryProcessor.h>
#include <VASDK/Frame.h>
#include <spdlog/spdlog.h>

namespace va {

RecoveryProcessor::RecoveryProcessor(Config config)
    : _config(std::move(config)) {}

void RecoveryProcessor::processFrame(std::shared_ptr<transport::Frame> frame,
                                      pipeline::FlowDirection direction) {
    if (direction == pipeline::FlowDirection::UPSTREAM) {
        if (frame->getType() == transport::FrameType::ERROR) {
            auto err = std::static_pointer_cast<transport::ErrorFrame>(frame);
            handleError(err);
            // Still forward the error upstream so other processors can observe it
        }
    }
    // Pass all frames through in their original direction
    if (direction == pipeline::FlowDirection::DOWNSTREAM) {
        pushDownstream(frame);
    } else {
        pushUpstream(frame);
    }
}

void RecoveryProcessor::handleError(const std::shared_ptr<transport::ErrorFrame>& err) {
    if (err->severity == transport::ErrorSeverity::FATAL) {
        spdlog::error("[RecoveryProcessor] FATAL error received - escalating. Message: {}",
                      err->errorMessage);
        emitRecoveryPrompt(_config.fatalPrompt);
        triggerFatal();
        return;
    }

    // Heuristic: classify error source from errorCode ranges or message keywords
    // STT errors: code 1xx  |  LLM errors: code 2xx  |  TTS errors: code 3xx
    // If no structured code, fall through to generic STT handling.
    const auto code = err->errorCode;
    const bool isLLM = (code >= 200 && code < 300);
    const bool isTTS = (code >= 300 && code < 400);

    if (isTTS) {
        int failures = ++_ttsFailures;
        spdlog::warn("[RecoveryProcessor] TTS error #{}: {}", failures, err->errorMessage);
        if (failures > _config.maxTtsRetries) {
            emitRecoveryPrompt(_config.fatalPrompt);
            triggerFatal();
        }
        // No recovery prompt on first TTS failure - TTS processor may self-recover
        return;
    }

    if (isLLM) {
        int failures = ++_llmFailures;
        spdlog::warn("[RecoveryProcessor] LLM error #{}: {}", failures, err->errorMessage);
        if (failures > _config.maxLlmRetries) {
            emitRecoveryPrompt(_config.fatalPrompt);
            triggerFatal();
        } else {
            emitRecoveryPrompt(_config.llmRetryPrompt);
        }
        return;
    }

    // Default: treat as STT error
    int failures = ++_sttFailures;
    spdlog::warn("[RecoveryProcessor] STT/unknown error #{}: {}", failures, err->errorMessage);
    if (failures > _config.maxSttRetries) {
        emitRecoveryPrompt(_config.fatalPrompt);
        triggerFatal();
    } else {
        emitRecoveryPrompt(_config.sttRetryPrompt);
    }
}

void RecoveryProcessor::emitRecoveryPrompt(const std::string& text) {
    auto frame  = std::make_shared<transport::TextFrame>();
    frame->text = text;
    pushDownstream(frame);
}

void RecoveryProcessor::triggerFatal() {
    if (_config.onFatalError) {
        _config.onFatalError();
    }
}

} // namespace va
