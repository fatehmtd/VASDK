#ifndef TURN1MINI_PROVIDER_H
#define TURN1MINI_PROVIDER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/CommonAudioUtils.h>
#include <VASDK/TurnDetectionProvider.h>

#include "vasdk_turn1mini_export.h"

namespace va::thirdparty::turn {

class Turn1MiniStream;

/** Settings of Turn1MiniProvider; every field has a default. */
struct Turn1MiniOptions {
    /** A turn-1-mini step graph: turn_1_mini.onnx (onnx/turn-1-mini.step.int8dyn.onnx, downloaded by
     *  VASDK_3P_IMPL/cmake/DownloadTurn1MiniModel.cmake), or any other turn-1-mini.step*.onnx. */
    std::string modelPath;
    /** End-of-turn score (0-1) that ends the turn after the VAD's end. The scores are not calibrated
     *  probabilities; after the VAD's end the model is held to no more than in a pause. */
    float threshold = 0.15f;
    /** Audio frames after the VAD's end that are still evaluated. */
    int postAudioFrames = 3;
    /** Also evaluate while the user pauses, so a clearly finished turn ends at the pause
     *  (TurnDetectionProvider::setEvaluateOnPause). */
    bool evaluateOnPause = true;
    /** End-of-turn score (0-1) a pause needs to end the turn (TurnDetectionProvider::setPauseThreshold).
     *  0.15: the best accuracy on the turn evaluation set (docs/providers/turn-1-mini.md, "Evaluation"). */
    float pauseThreshold = 0.15f;
};

/**
 * turn-1-mini (p99lab): a 5.4M-parameter streaming end-of-turn model, audio only, English.
 *
 * Unlike a model that re-reads the turn when asked, it hears all the audio: every AUDIO frame goes into
 * the stream (Turn1MiniStream), one 160 ms step at a time, about 1 ms of CPU per step with the default
 * graph. Its decisions follow the same rules as SmartTurnV3Provider:
 *   - TURN_START when the VAD starts (INTERRUPT while the agent speaks, NORMAL otherwise);
 *   - during a pause (VAD PAUSED or ENDING, with evaluateOnPause), TURN_END when the score of "now"
 *     reaches pauseThreshold;
 *   - after the VAD's end, for postAudioFrames frames, TURN_END when it reaches threshold.
 *
 * Usage:
 *   Turn1MiniOptions options;
 *   options.modelPath = modelsDir + "/turn_1_mini.onnx";
 *   auto provider = std::make_shared<Turn1MiniProvider>(options);
 *   provider->initialize(audioConfig, ringBuffer);   // 16 kHz mono
 *   auto turnProcessor = std::make_shared<TurnDetectionProcessor>(provider);
 */
class VASDK_TURN1MINI_EXPORT Turn1MiniProvider : public va::audio::TurnDetectionProvider {
public:
    explicit Turn1MiniProvider(Turn1MiniOptions options = {});
    ~Turn1MiniProvider() override;

    /** Loads the model. Needs 16 kHz mono audio and the ring buffer the audio frames point into. */
    bool initialize(const va::audio::AudioConfig& audioConfig, std::shared_ptr<va::AudioRingBuffer> ringBuffer);

    TurnEvaluation evaluate(const std::shared_ptr<va::transport::Frame> frame, bool agentIsSpeaking) override;

    /** Starts a new stream (a new call): the model forgets the audio it heard. */
    void reset();

    /** The options it runs with. */
    const Turn1MiniOptions& options() const { return _options; }

private:
    TurnEvaluation handleVadState(const va::transport::VADStateFrame& frame, bool agentIsSpeaking);
    TurnEvaluation handleAudio(const va::transport::AudioFrame& frame);
    // Reads the frame's samples from the ring buffer into the stream
    bool hear(const va::transport::AudioFrame& frame);

    Turn1MiniOptions _options;
    std::unique_ptr<Turn1MiniStream> _stream;
    std::shared_ptr<va::AudioRingBuffer> _ringBuffer;
    va::audio::AudioConfig _audioConfig;
    std::vector<char> _frameBytes;
    std::vector<float> _frameSamples;

    bool _vadActive{false};
    bool _vadOccurred{false};
    bool _paused{false};   // VAD PAUSED or ENDING
    int _postAudioFramesProcessed{0};
};

} // namespace va::thirdparty::turn

#endif
