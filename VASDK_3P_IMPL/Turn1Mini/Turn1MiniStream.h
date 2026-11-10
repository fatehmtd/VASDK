#ifndef TURN1MINI_STREAM_H
#define TURN1MINI_STREAM_H

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "vasdk_turn1mini_export.h"

namespace va::thirdparty::turn {

/**
 * One audio stream through turn-1-mini's ONNX step graph (p99lab/turn-1-mini, onnx/turn-1-mini.step*.onnx).
 *
 * A step takes 160 ms of 16 kHz mono audio (2,560 samples in [-1, 1]) and the 7 state tensors of the
 * previous step, and gives 8 end-of-turn scores (one per 20 ms frame, oldest first) and the new state.
 * The 4 kHz low-pass and the log-mel are inside the graph. The state starts as zeros (the start of a
 * stream) and is fed back unchanged after every step; it holds the last 1.2 s of context, so a stream
 * can run for as long as the session does.
 *
 * Not thread-safe: one caller at a time.
 */
class VASDK_TURN1MINI_EXPORT Turn1MiniStream {
public:
    /** Samples per step: 160 ms at 16 kHz. */
    static constexpr int kStepSamples = 2560;
    /** Scores per step: one per 20 ms frame. */
    static constexpr int kScoresPerStep = 8;
    /** The state tensors, in the graph's order: input state_<name>, output new_state_<name>. */
    static constexpr std::array<const char*, 7> kStateNames = {"audio", "conv1", "conv2", "k", "v", "k_valid", "h6"};

    /** @param modelPath one of the turn-1-mini.step*.onnx graphs. */
    explicit Turn1MiniStream(std::string modelPath);
    ~Turn1MiniStream();
    Turn1MiniStream(const Turn1MiniStream&) = delete;
    Turn1MiniStream& operator=(const Turn1MiniStream&) = delete;

    /** Loads the graph and checks its inputs and outputs; false (and a log line) when it can't. */
    bool initialize();

    /**
     * Appends samples and runs one step per full 2,560 of them.
     * @param scores when given, every score of the steps run is appended to it, oldest first.
     * @return the number of steps run.
     */
    int push(const float* samples, size_t count, std::vector<float>* scores = nullptr);

    /** The most recent frame's score of the last full step; 0 before the first step. */
    float lastScore() const { return _lastScore; }

    /**
     * The score of "now", between two steps: a step on the samples still pending, padded with zeros,
     * whose state is thrown away (the stream's own state is untouched). With nothing pending, lastScore().
     */
    float scoreNow();

    /** Samples waiting for the next full step. */
    size_t pendingSamples() const { return _pending; }

    /** Starts a new stream: zero state, nothing pending. */
    void reset();

private:
    // Runs the graph on _stepAudio; keeps the new state only when keepState
    float runStep(bool keepState, std::vector<float>* scores);

    std::string _modelPath;
    Ort::Env _environment{nullptr};
    std::unique_ptr<Ort::Session> _session;
    Ort::MemoryInfo _memoryInfo{nullptr};

    // Inputs: [0] audio over _stepAudio, then the 7 states; outputs: probs, then the 7 new states
    std::vector<std::string> _inputNameStorage;
    std::vector<std::string> _outputNameStorage;
    std::vector<const char*> _inputNames;
    std::vector<const char*> _outputNames;
    std::vector<std::vector<int64_t>> _stateShapes;
    std::vector<Ort::Value> _inputs;

    std::vector<float> _stepAudio;   // kStepSamples, the audio tensor's buffer
    size_t _pending{0};              // samples of _stepAudio filled so far
    float _lastScore{0.0f};
};

} // namespace va::thirdparty::turn

#endif
