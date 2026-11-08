#include "MossVoiceSynthesisProvider.hpp"
#include "impl/MossTTSImpl.hpp"

#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace va::thirdparty::tts {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MossVoiceSynthesisProvider::MossVoiceSynthesisProvider(MossVoiceSynthesisOptions options)
    : QueuedVoiceSynthesisProvider("MossTTS", Delivery::Thread),
      _impl(std::make_shared<MossTTSImpl>(options.modelDir, options.voice, options.threads))
{
    auto params = _impl->getParams();
    params.silenceDuration = std::max(0.0f, options.silenceDurationSeconds);
    params.maxNewFrames    = std::max(0, options.maxNewFrames);
    params.maxChunkTokens  = std::max(1, options.maxChunkTokens);
    params.normalizeText   = options.textNormalization;
    params.pauseShortSeconds = std::max(0.0f, options.pauseShortSeconds);
    params.pauseLongSeconds  = std::max(0.0f, options.pauseLongSeconds);
    _impl->setParams(params);
    setPreBufferedSamplesMS(std::max(0, options.prebufferMs));
}

MossVoiceSynthesisProvider::~MossVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

bool MossVoiceSynthesisProvider::loadEngine() {
    if (!_impl->initialize()) {
        spdlog::error("[MossTTS] Initialization failed.");
        return false;
    }
    _modelSampleRate = _impl->getSampleRate();
    spdlog::info("[MossTTS] Model rate {} Hz, voice {}", _modelSampleRate, getVoice());
    return true;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

bool MossVoiceSynthesisProvider::setVoice(const std::string& voiceName) {
    auto p = _impl->getParams();
    p.voiceName = voiceName;
    _impl->setParams(p);
    const auto names = _impl->getVoiceNames();
    const bool found = std::find(names.begin(), names.end(), voiceName) != names.end();
    if (!found && !names.empty())
        spdlog::warn("[MossTTS] Voice '{}' not in manifest; will fall back to '{}'.", voiceName, names.front());
    return found;
}

std::string MossVoiceSynthesisProvider::getVoice() const {
    return _impl->getParams().voiceName;
}

std::vector<std::string> MossVoiceSynthesisProvider::getAvailableVoices() const {
    return _impl->getVoiceNames();
}

// ---------------------------------------------------------------------------
// synthesizeOne - runs ONNX inference, resamples, hands chunks to the delivery thread
// ---------------------------------------------------------------------------

bool MossVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
    // A part in another of its voices (say { voice }): that voice for this text only.
    struct VoiceFor {
        decltype(_impl) impl;
        std::string own;
        VoiceFor(decltype(_impl) engine, const std::string& wanted) : impl(std::move(engine)) {
            auto p = impl->getParams();
            if (wanted.empty() || wanted == p.voiceName) return;
            own = p.voiceName;
            p.voiceName = wanted;
            impl->setParams(p);
        }
        ~VoiceFor() {
            if (own.empty()) return;
            auto p = impl->getParams();
            p.voiceName = own;
            impl->setParams(p);
        }
    } voiceFor(_impl, style().voice);
    VASDK_CTRACK_NAME("MossTTS::synthesize");
    _resampler.reset(_modelSampleRate, audioConfig().sampleRate);
    const int outRate = _resampler.passthrough() ? _modelSampleRate : audioConfig().sampleRate;

    _impl->synthesize(text, cancelFlag(), [&](std::vector<float> chunk) {
        if (cancelled()) return;
        if (_resampler.passthrough()) { handoffAudio(std::move(chunk), outRate); return; }
        std::vector<float> out;
        _resampler.process(chunk.data(), chunk.size(), out);
        handoffAudio(std::move(out), outRate);
    });

    if (!cancelled()) {
        std::vector<float> tail;
        _resampler.flush(tail);
        handoffAudio(std::move(tail), outRate);
    }
    return finishHandoff();
}

} // namespace va::thirdparty::tts
