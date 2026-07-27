#pragma once
#include <iostream>
#include <vector>
#include <sstream>
#include <cstring>
#include <limits>
#include <chrono>
#include <iomanip>
#include <memory>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <cstdarg>
#include <cmath>    // for std::rint
#include <algorithm> // for std::remove_if
#if __cplusplus < 201703L
#include <memory>
#endif

#include <onnxruntime_cxx_api.h>
#ifdef VASDK_USE_NNAPI
#include <nnapi_provider_factory.h>
#endif

// timestamp_t class: stores the start and end (in samples) of a speech segment.
class timestamp_t {
public:
    int start;
    int end;
    float probability;

    timestamp_t(int start = -1, int end = -1, float probability = 0.0f)
        : start(start), end(end), probability(probability) { }

    timestamp_t& operator=(const timestamp_t& a) {
        start = a.start;
        end = a.end;
        probability = a.probability;
        return *this;
    }

    bool operator==(const timestamp_t& a) const {
        return (start == a.start && end == a.end && probability == a.probability);
    }

    std::string c_str() const {
        return format("{start:%08d, end:%08d, probability:%.2f}", start, end, probability);
    }
private:
    std::string format(const char* fmt, ...) const {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        const auto r = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (r < 0)
            return {};
        const size_t len = r;
        if (len < sizeof(buf))
            return std::string(buf, len);
#if __cplusplus >= 201703L
        std::string s(len, '\0');
        va_start(args, fmt);
        std::vsnprintf(s.data(), len + 1, fmt, args);
        va_end(args);
        return s;
#else
        auto vbuf = std::unique_ptr<char[]>(new char[len + 1]);
        va_start(args, fmt);
        std::vsnprintf(vbuf.get(), len + 1, fmt, args);
        va_end(args);
        return std::string(vbuf.get(), len);
#endif
    }
};

// VadIterator class: uses ONNX Runtime to detect speech segments.
// Mirrors the two-phase architecture of silero.cc v1.3:
//   Phase 1 - SpeechProbs(): run ONNX inference per chunk, accumulate probabilities.
//   Phase 2 - DoVad(): apply the VAD state machine over the collected probabilities.
class VadIterator {
private:
    // ONNX Runtime resources
    Ort::Env env;
    Ort::SessionOptions session_options;
    std::shared_ptr<Ort::Session> session = nullptr;
    Ort::AllocatorWithDefaultOptions allocator;
    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPU);

    // Context window: context_samples = window_size_samples / 8 (dynamic, set in init_engine)
    int context_samples = 0;
    std::vector<float> _context;

    int window_size_samples = 0;
    int effective_window_size = 0;

    // ONNX Runtime input/output buffers
    std::vector<Ort::Value> ort_inputs;
    std::vector<const char*> input_node_names = { "input", "state", "sr" };
    std::vector<float> input;
    unsigned int size_state = 2 * 1 * 128;
    std::vector<float> _state;
    std::vector<int64_t> sr;
    int64_t input_node_dims[2] = {};
    const int64_t state_node_dims[3] = { 2, 1, 128 };
    const int64_t sr_node_dims[1] = { 1 };
    std::vector<Ort::Value> ort_outputs;
    std::vector<const char*> output_node_names = { "output", "stateN" };

    // Model configuration parameters
    int sample_rate;
    float threshold;
    int min_silence_samples;
    int min_speech_samples;
    int speech_pad_samples;

    // Stored ms values needed by init_engine
    int window_size_ms_;
    int min_silence_duration_ms_;
    int speech_pad_ms_;
    int min_speech_duration_ms_;

    // State
    bool triggered = false;
    unsigned int temp_end = 0;
    unsigned int current_sample = 0;
    int total_sample_size = 0;
    std::vector<float> outputs_prob;

    // Initializes engine parameters and ONNX buffer sizes.
    void init_engine(int window_size_ms) {
        window_size_samples  = sample_rate / 1000 * window_size_ms;
        context_samples      = window_size_samples / 8;
        effective_window_size = window_size_samples + context_samples;
        min_silence_samples  = sample_rate * min_silence_duration_ms_ / 1000;
        speech_pad_samples   = sample_rate * speech_pad_ms_ / 1000;
        min_speech_samples   = sample_rate * min_speech_duration_ms_ / 1000;

        _context.assign(context_samples, 0.0f);
        input.assign(effective_window_size, 0.0f); // pre-allocate inference input buffer
        input_node_dims[0] = 1;
        input_node_dims[1] = effective_window_size;
        _state.resize(size_state, 0.0f);
        sr.resize(1);
        sr[0] = sample_rate;
    }

    // Loads the ONNX model.
    void init_onnx_model(const std::string& model_path) {
        session_options.SetIntraOpNumThreads(1);
        session_options.SetInterOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef VASDK_USE_NNAPI
        OrtSessionOptionsAppendExecutionProvider_Nnapi(session_options, 0);
#endif
#ifdef _WIN32
        std::wstring w_model_path(model_path.begin(), model_path.end());
        session = std::make_shared<Ort::Session>(env, w_model_path.c_str(), session_options);
#else
        session = std::make_shared<Ort::Session>(env, model_path.c_str(), session_options);
#endif
    }

    // Resets per-call detection bookkeeping only. Preserves ONNX hidden state
    // and context so temporal continuity is maintained across streaming calls.
    void reset_detection() {
        triggered = false;
        current_sample = 0;
        temp_end = 0;
        outputs_prob.clear();
        total_sample_size = 0;
    }

    // Full reset including ONNX hidden state and context (used by public reset()).
    void reset_states() {
        reset_detection();
        std::memset(_state.data(), 0, _state.size() * sizeof(float));
        std::fill(_context.begin(), _context.end(), 0.0f);
    }

    // Runs ONNX inference on one chunk, always updates context, returns speech probability.
    // data_chunk must point to exactly chunk_size (== window_size_samples) valid floats.
    float predict(const float* data_chunk, int chunk_size) {
        // Build input in-place: context prefix + new chunk (no intermediate allocation)
        std::copy(_context.begin(), _context.end(), input.begin());
        std::copy(data_chunk, data_chunk + chunk_size, input.begin() + context_samples);

        Ort::Value input_ort = Ort::Value::CreateTensor<float>(
            memory_info, input.data(), input.size(), input_node_dims, 2);
        Ort::Value state_ort = Ort::Value::CreateTensor<float>(
            memory_info, _state.data(), _state.size(), state_node_dims, 3);
        Ort::Value sr_ort = Ort::Value::CreateTensor<int64_t>(
            memory_info, sr.data(), sr.size(), sr_node_dims, 1);
        ort_inputs.clear();
        ort_inputs.emplace_back(std::move(input_ort));
        ort_inputs.emplace_back(std::move(state_ort));
        ort_inputs.emplace_back(std::move(sr_ort));

        ort_outputs = session->Run(
            Ort::RunOptions{ nullptr },
            input_node_names.data(), ort_inputs.data(), ort_inputs.size(),
            output_node_names.data(), output_node_names.size());

        float speech_prob = ort_outputs[0].GetTensorMutableData<float>()[0];
        float* stateN = ort_outputs[1].GetTensorMutableData<float>();
        std::memcpy(_state.data(), stateN, size_state * sizeof(float));

        // Always update context with the tail of the new input.
        std::copy(input.end() - context_samples, input.end(), _context.begin());

        return speech_prob;
    }

    // Phase 1: run inference on all chunks and accumulate probabilities.
    void SpeechProbs(const std::vector<float>& input_wav, int numSamples) {
        reset_detection(); // Preserve _state/_context for streaming continuity
        total_sample_size = numSamples;
        for (size_t j = 0; j < static_cast<size_t>(numSamples); j += static_cast<size_t>(window_size_samples)) {
            if (j + static_cast<size_t>(window_size_samples) > static_cast<size_t>(numSamples))
                break;
            outputs_prob.push_back(predict(&input_wav[j], window_size_samples));
        }
    }

    // Phase 2: apply the VAD state machine over the accumulated probabilities.
    std::vector<timestamp_t> DoVad() {
        std::vector<timestamp_t> speeches;

        float speech_prob = 0.0f;
        for (size_t i = 0; i < outputs_prob.size(); ++i) {
            speech_prob = outputs_prob[i];
            current_sample += static_cast<unsigned int>(window_size_samples);

            if (speech_prob >= threshold && temp_end != 0)
                temp_end = 0;

            if (speech_prob >= threshold) {
                if (!triggered) {
                    triggered = true;
                    timestamp_t seg;
                    seg.start = std::max(0, static_cast<int>(current_sample) - speech_pad_samples - window_size_samples);
                    seg.probability = speech_prob;
                    speeches.push_back(seg);
                }
            } else {
                if (triggered) {
                    if (speech_prob < threshold - 0.15f) {
                        if (temp_end == 0)
                            temp_end = current_sample;
                        if (static_cast<int>(current_sample - temp_end) >= min_silence_samples) {
                            speeches.back().end = static_cast<int>(temp_end) + speech_pad_samples - window_size_samples;
                            speeches.back().probability = std::max(speeches.back().probability, speech_prob);
                            temp_end = 0;
                            triggered = false;
                        }
                    }
                }
            }
        }

        // Finalize any active speech segment at the end of the buffer.
        if (triggered) {
            speeches.back().end = total_sample_size;
            speeches.back().probability = std::max(speeches.back().probability, speech_prob);
            triggered = false;
        }

        // No short-segment filter here: VADFrameProcessor enforces minSpeechDurationMs
        // above this layer. Filtering here would discard valid streaming segments that
        // are shorter than min_speech_duration_ms (e.g. a 250ms buffer of pure speech).

        reset_detection(); // Clear per-call state; _state/_context persist for next call
        return speeches;
    }

public:
    // Phase 1: run inference and populate internal probability buffer.
    void process(const std::vector<float>& input_wav, int numSamples) {
        SpeechProbs(input_wav, numSamples);
    }

    // Phase 2: apply VAD state machine and return detected speech segments (in samples).
    std::vector<timestamp_t> get_speech_timestamps() {
        return DoVad();
    }

    // Resets the internal state (ONNX hidden state, context, bookkeeping).
    void reset() {
        reset_states();
    }

public:
    VadIterator(const std::string& ModelPath,
        int Sample_rate        = 16000,
        int windows_frame_size = 32,
        float Threshold        = 0.5f,
        int min_silence_duration_ms = 100,
        int speech_pad_ms           = 30,
        int min_speech_duration_ms  = 250)
        : sample_rate(Sample_rate),
          threshold(Threshold),
          window_size_ms_(windows_frame_size),
          min_silence_duration_ms_(min_silence_duration_ms),
          speech_pad_ms_(speech_pad_ms),
          min_speech_duration_ms_(min_speech_duration_ms),
          min_silence_samples(0),
          min_speech_samples(0),
          speech_pad_samples(0)
    {
        init_engine(windows_frame_size);
        init_onnx_model(ModelPath);
    }
};
