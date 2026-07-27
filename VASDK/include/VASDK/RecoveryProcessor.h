#pragma once

#include "vasdk_export.h"
#include <VASDK/FrameProcessor.h>
#include <atomic>
#include <functional>
#include <string>

namespace va {

/// Observes ErrorFrame events flowing upstream and responds with:
///   - A TTS recovery prompt pushed downstream (caller hears a friendly message)
///   - Retry tracking per error source
///   - Escalation trigger when max retries are exhausted or a FATAL error arrives
///
/// Insert one instance between TranscriptionProcessor and AgentStateProcessor to
/// catch STT errors, and optionally a second instance after TTSFrameProcessor for
/// TTS / LLM errors.  A single instance placed at the head of the pipeline also
/// works since ErrorFrames flow upstream past all processors.
class VASDK_EXPORT RecoveryProcessor : public pipeline::FrameProcessor {
public:
    struct Config {
        int maxSttRetries = 2;
        int maxLlmRetries = 2;
        int maxTtsRetries = 1;

        std::string sttRetryPrompt = "I'm sorry, I didn't catch that - could you say that again?";
        std::string llmRetryPrompt = "Bear with me one moment…";
        std::string fatalPrompt    = "I'm having some technical difficulties. "
                                     "Let me connect you with an agent.";

        /// Fired when max retries are exceeded or a FATAL error arrives.
        /// App should trigger escalation (e.g. call GoalManager's onEscalate).
        std::function<void()> onFatalError;
    };

    explicit RecoveryProcessor(Config config);

    std::string getName() const override { return "RecoveryProcessor"; }
    void processFrame(std::shared_ptr<transport::Frame> frame,
                      pipeline::FlowDirection direction) override;

private:
    Config        _config;
    std::atomic<int> _sttFailures{0};
    std::atomic<int> _llmFailures{0};
    std::atomic<int> _ttsFailures{0};

    void handleError(const std::shared_ptr<transport::ErrorFrame>& err);
    void emitRecoveryPrompt(const std::string& text);
    void triggerFatal();
};

} // namespace va
