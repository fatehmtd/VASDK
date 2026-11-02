#pragma once
#include "TypeName.h"

#include "Frame.h"
#include <memory>

namespace va {
namespace audio {

/**
 * @brief Abstract interface for turn detection providers.
 *
 * A turn detection provider evaluates each frame and decides,
 * with a confidence score, whether a conversational turn has started or
 * ended - for either the user (USER actor) or the agent (SYSTEM actor).
 * The TurnDetectionProcessor delegates all decision-making to this
 * interface, keeping acoustic boundary detection separate from the
 * heuristic or ML logic that computes it.
 *
 * Built-in (simple) behaviour:
 *   VAD STARTING  →  USER turn start  (confidence = VAD confidence)
 *   VAD ENDED     →  USER turn end    (confidence = VAD confidence)
 *
 * Smart implementations (e.g. SmartTurnDetection in VASDK_3P_IMPL) can:
 *   - apply configurable confidence thresholds
 *   - run a rolling average over several VAD frames to reduce flicker
 *   - classify the turn type (NORMAL / INTERRUPT / OVERLAP) by checking
 *     whether the agent is currently speaking
 *   - suppress very-short speech bursts below a minimum duration
 *   - attribute turns to SYSTEM when appropriate (e.g. agent self-interrupts)
 */
class TurnDetectionProvider {
public:

    /**
     * @brief The turn event decision returned by evaluate().
     *
     * NONE      - no turn boundary detected this frame, do nothing.
     * TURN_START - emit a TurnStateFrame with TurnState::START.
     * TURN_END   - emit a TurnStateFrame with TurnState::END.
     */
    enum class Decision { NONE, TURN_START, TURN_END };

    /**
     * @brief Result returned by evaluate() for each frame.
     *
     * Contains everything the TurnDetectionProcessor needs to construct
     * and emit a TurnStateFrame - covering both user and agent turns.
     */
    struct TurnEvaluation {
        Decision decision   { Decision::NONE };                              // What event to emit
        float    confidence { 0.0f };                                        // Confidence score (0.0 – 1.0)
        transport::TurnStateFrame::TurnActor actor { transport::TurnStateFrame::TurnActor::USER };   // Who is taking the turn
        transport::TurnStateFrame::TurnType  type  { transport::TurnStateFrame::TurnType::NORMAL };  // Turn classification
    };

    virtual ~TurnDetectionProvider() = default;
    virtual std::string getName() const { return shortTypeName(typeid(*this)); }  ///< for logs and UIs

    //  Core evaluation 

    /**
     * @brief Evaluate a frame and return a turn decision.
     *
     * @param frame           The incoming frame.
     * @param agentIsSpeaking True when the agent's TTS is currently playing.
     *                        Used to classify INTERRUPT vs NORMAL turns.
     * @return TurnEvaluation  The turn decision and confidence score.
     */
    virtual TurnEvaluation evaluate(
        const std::shared_ptr<transport::Frame> frame,
        bool agentIsSpeaking) = 0;

    //  Early end of turn

    /**
     * @brief Also score the user's turn while the VAD is PAUSED/ENDING, so a confident turn end
     *        fires at the pause instead of after the VAD's end timers. TurnDetectionProcessor then
     *        asks the VAD to close the segment early. Default off; providers that can't score a
     *        pause ignore it.
     */
    void setEvaluateOnPause(bool enabled) { _evaluateOnPause = enabled; }
    bool evaluateOnPause() const { return _evaluateOnPause; }

    /**
     * @brief Confidence needed to end the turn during a pause (see setEvaluateOnPause). Kept above
     *        the provider's regular end threshold: the user is often only taking a breath, and an
     *        uncertain turn still ends once the VAD confirms silence. Default 0.85.
     */
    void setPauseThreshold(float threshold) { _pauseThreshold = threshold; }
    float pauseThreshold() const { return _pauseThreshold; }

protected:
    bool  _evaluateOnPause{ false };
    float _pauseThreshold{ 0.85f };
};

} // namespace audio
} // namespace va
