#pragma once

#include "FrameProcessor.h"
#include "llm/LLMFirePolicy.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace va {
namespace pipeline {

/**
 * @brief Turns transcriptions and turn-end signals into one UserTurnFrame per user turn.
 *
 * Place it after AgentStateProcessor and before whatever answers the user (LLMProcessor).
 * All frames pass through unchanged; a UserTurnFrame follows the frame that completed the turn.
 *
 * A turn ends when both the user's turn end (USER_STOPPED_SPEAKING or TurnStateFrame END) and
 * its final transcription have arrived, in either order. Finals that arrive while the turn is
 * open are joined. If the final never comes, the watchdog sends the final staged so far, or the
 * latest partial when the fire policy allows. With an eager fire policy the turn can also be
 * sent on the partial at TRANSCRIPTION_READY; its late final is then dropped.
 *
 * Resumed turns: when the user picks up again after a pause that ended their turn, their earlier
 * words go in front of the next turn, so "Book a flight ... to Paris" is one turn. Words that were
 * never sent are always kept. A turn already sent is merged (and the UserTurnFrame marked
 * replacesPrevious) when the user resumes while the agent is still thinking, or barges in within
 * setResumeMergeMs() of it. A TranscriptionFrame{isFollowUp} announces the merged text.
 */
class VASDK_EXPORT TurnAssemblerProcessor : public FrameProcessor {
public:
    TurnAssemblerProcessor();
    ~TurnAssemblerProcessor() override;

    std::string getName() const override { return "TurnAssembler"; }
    bool start() override;
    void stop() override;
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

    /** Whether a partial may stand in for the final (default: eager). Set before start(). */
    void setFirePolicy(std::shared_ptr<llm::LLMFirePolicyInterface> policy) {
        if (policy) _firePolicy = std::move(policy);
    }
    const llm::LLMFirePolicyInterface& firePolicy() const { return *_firePolicy; }

    /** How long to wait for the final transcription after the turn ended (default 2000 ms). */
    void setFinalWatchdogTimeoutMs(uint32_t ms) { _finalWatchdogTimeoutMs = ms; }

    /** Barge-in window for merging a resumed turn into the one already sent (default 3000 ms).
     *  0 turns resumed-turn merging off entirely, including words never sent. */
    void setResumeMergeMs(uint32_t ms) { _resumeMergeMs = ms; }
    [[deprecated("use setResumeMergeMs")]] void setFollowUpWindowMs(uint32_t ms) { setResumeMergeMs(ms); }

private:
    void handleAgentState(const transport::AgentStateFrame& frame);
    void handleFinal(const transport::TranscriptionFrame& frame);
    void handleUserStoppedSpeaking();
    void handleTurnStateEnd();
    void handleTranscriptionReady();
    void handleInterrupt(transport::AgentStateTransition transition);
    // Moves a final staged for a turn end that never came in front of the next turn. Holds _mutex.
    void keepUnsentTurn();
    // Sends the partial as the turn if the fire policy lets it stand in for the final.
    void emitPartialIfAllowed(const std::string& text, float confidence, const char* why);
    void emitTurn(const std::string& said, float confidence);

    void startFinalWatchdog();
    void cancelFinalWatchdog();
    void finalWatchdogLoop();

    std::shared_ptr<llm::LLMFirePolicyInterface> _firePolicy{std::make_shared<llm::EagerFirePolicy>()};
    uint32_t _finalWatchdogTimeoutMs{2000};
    uint32_t _resumeMergeMs{3000};

    std::mutex  _mutex;   // everything below, except the watchdog's own state
    std::string _latestPartialText;
    float       _latestPartialConfidence{0.0f};
    bool        _pendingTurnEnd{false};       // a final or a turn end arrived; waiting for the other
    std::string _pendingFallbackText;          // the partial when the turn ended
    float       _pendingFallbackConfidence{0.0f};
    std::string _accumulatedFinalText;
    float       _accumulatedFinalConfidence{0.0f};
    bool        _partialAlreadySent{false};   // sent on the partial: drop the turn's late final
    std::string _lastTurnText;                // last turn sent, for merging a resumed one
    std::chrono::steady_clock::time_point _lastTurnAt;
    std::string _carriedText;                 // earlier words to put in front of the next turn
    bool        _carriedReplacesPrevious{false};
    uint64_t    _nextTurnId{1};

    std::thread             _watchdogThread;
    std::mutex              _watchdogMutex;
    std::condition_variable _watchdogCv;
    bool                    _watchdogArmed{false};
    bool                    _watchdogStop{false};
};

} // namespace pipeline
} // namespace va
