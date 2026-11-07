#pragma once

#include "moonshine-streaming-model.h"
#include <spdlog/spdlog.h>
#include <VASDK/CommonAudioUtils.h>
#include <VASDK/metrics/MetricsCollector.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace va::thirdparty {

// ---------------------------------------------------------------------------
// Helpers (file-local, defined inline so multiple TUs can include this header)
// ---------------------------------------------------------------------------

namespace moonshine_impl {

inline size_t convertToFloatInto(const uint8_t* data, size_t size,
                                  va::audio::AudioFormat format,
                                  float* out, size_t outCapacity)
{
    switch (format) {
    case va::audio::AudioFormat::SHORT: {
        if (size % sizeof(int16_t) != 0) size -= size % sizeof(int16_t);
        size_t n = std::min(size / sizeof(int16_t), outCapacity);
        for (size_t i = 0; i < n; ++i) {
            int16_t s;
            std::memcpy(&s, data + i * sizeof(int16_t), sizeof(int16_t));
            out[i] = static_cast<float>(s) / 32768.0f;
        }
        return n;
    }
    case va::audio::AudioFormat::FLOAT: {
        if (size % sizeof(float) != 0) size -= size % sizeof(float);
        size_t n = std::min(size / sizeof(float), outCapacity);
        std::memcpy(out, data, n * sizeof(float));
        return n;
    }
    case va::audio::AudioFormat::INT: {
        if (size % sizeof(int32_t) != 0) size -= size % sizeof(int32_t);
        size_t n = std::min(size / sizeof(int32_t), outCapacity);
        for (size_t i = 0; i < n; ++i) {
            int32_t s;
            std::memcpy(&s, data + i * sizeof(int32_t), sizeof(int32_t));
            out[i] = static_cast<float>(s) / 2147483648.0f;
        }
        return n;
    }
    case va::audio::AudioFormat::BYTE: {
        size_t n = std::min(size, outCapacity);
        for (size_t i = 0; i < n; ++i)
            out[i] = (static_cast<float>(data[i]) - 128.0f) / 128.0f;
        return n;
    }
    default:
        spdlog::error("[MoonshineStt] Unknown audio format");
        return 0;
    }
}

} // namespace moonshine_impl

// ---------------------------------------------------------------------------
// MoonshineSttProviderImpl
// Thin wrapper over Moonshine's own MoonshineStreamingModel (compiled from the
// upstream core sources against our ONNX Runtime). One model, many streams:
// model calls lock internally, and each Stream owns its incremental state.
// ---------------------------------------------------------------------------

class MoonshineSttProviderImpl {
public:
    // Incremental encoder/decoder state for one utterance.
    class Stream {
    public:
        explicit Stream(MoonshineSttProviderImpl& owner)
            : _owner(owner), _state(owner._model->create_state()) {}
        ~Stream() { delete _state; }
        Stream(const Stream&)            = delete;
        Stream& operator=(const Stream&) = delete;

        // Feeds 16 kHz float PCM and runs the encoder over complete chunks.
        bool addAudio(const float* samples, size_t n)
        {
            _pending.insert(_pending.end(), samples, samples + n);
            const size_t chunks = _pending.size() / kChunkSamples;
            if (chunks == 0) return true;

            auto& model = *_owner._model;
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < chunks; ++i) {
                if (model.process_audio_chunk(_state, _pending.data() + i * kChunkSamples,
                                              kChunkSamples, nullptr) != 0) {
                    spdlog::error("[MoonshineStt] process_audio_chunk failed");
                    return false;
                }
            }
            _pending.erase(_pending.begin(), _pending.begin() + chunks * kChunkSamples);
            _hasAudio = true;
            int newFrames = 0;
            if (model.encode(_state, false, &newFrames) != 0) {
                spdlog::error("[MoonshineStt] encode failed");
                return false;
            }
            _owner.reportTiming("encoder", t0);
            return true;
        }

        // Decodes everything encoded so far; reuses the previous tokens as a speculative draft.
        std::string decode()
        {
            if (_state->memory_len == 0) return {};
            auto& model = *_owner._model;
            const auto t0 = std::chrono::steady_clock::now();
            model.decoder_reset(_state);

            std::vector<int> draft;
            for (int t : _lastTokens)
                if (t != model.config.bos_id && t != model.config.eos_id) draft.push_back(t);

            int* out = nullptr;
            int outLen = 0;
            if (model.decode_full(_state, draft.empty() ? nullptr : draft.data(),
                                  static_cast<int>(draft.size()), &out, &outLen) != 0) {
                spdlog::error("[MoonshineStt] decode_full failed");
                return {};
            }
            std::unique_ptr<int, decltype(&std::free)> owned(out, &std::free);
            _lastTokens.assign(out, out + outLen);
            _owner.reportTiming("decoder", t0);

            std::vector<int64_t> tokens(_lastTokens.begin(), _lastTokens.end());
            return model.tokens_to_text(tokens);
        }

        // Flushes the encoder lookahead, decodes the final text and resets for the next utterance.
        // The sub-chunk tail (< 80 ms) is dropped, as upstream does.
        std::string finish()
        {
            std::string text;
            int newFrames = 0;
            if (_owner._model->encode(_state, true, &newFrames) == 0)
                text = decode();
            else
                spdlog::error("[MoonshineStt] final encode failed");
            reset();
            return text;
        }

        void reset()
        {
            _state->reset(_owner._model->config);
            _pending.clear();
            _lastTokens.clear();
            _hasAudio = false;
        }

        bool hasAudio() const { return _hasAudio; }

    private:
        // Whole multiples of 80 samples are required: pre-2026-08-23 streaming graphs (the
        // English ones) silently drop a chunk's remainder. 1280 matches upstream's transcriber.
        static constexpr size_t kChunkSamples = 1280;

        MoonshineSttProviderImpl& _owner;
        MoonshineStreamingState*  _state;
        std::vector<float>        _pending;
        std::vector<int>          _lastTokens;
        bool                      _hasAudio = false;
    };

    MoonshineSttProviderImpl() = default;
    MoonshineSttProviderImpl(const MoonshineSttProviderImpl&)            = delete;
    MoonshineSttProviderImpl& operator=(const MoonshineSttProviderImpl&) = delete;

    bool initialize(const std::string& modelDir)
    {
        static const char* kFiles[] = { "frontend.ort", "encoder.ort", "adapter.ort", "cross_kv.ort",
                                        "decoder_kv.ort", "streaming_config.json", "tokenizer.bin" };
        for (const char* f : kFiles) {
            if (!std::filesystem::exists(std::filesystem::path(modelDir) / f)) {
                spdlog::error("[MoonshineStt] Model file not found: {}/{}", modelDir, f);
                return false;
            }
        }

        _model = std::make_unique<MoonshineStreamingModel>();
        const std::string tokenizerPath = modelDir + "/tokenizer.bin";
        if (int rc = _model->load(modelDir.c_str(), tokenizerPath.c_str(), 0); rc != 0) {
            spdlog::error("[MoonshineStt] Failed to load streaming model from {} (rc={})", modelDir, rc);
            _model.reset();
            return false;
        }
        return true;
    }

    void setTimingObserver(va::metrics::TimingObserverInterface* observer) { _timingObserver = observer; }

private:
    void reportTiming(const char* name, std::chrono::steady_clock::time_point t0)
    {
        if (_timingObserver)
            _timingObserver->onTiming(name, std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - t0).count());
    }

    std::unique_ptr<MoonshineStreamingModel> _model;
    va::metrics::TimingObserverInterface*    _timingObserver = nullptr;
};

} // namespace va::thirdparty
