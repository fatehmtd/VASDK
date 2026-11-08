#include "PocketVoiceSynthesisProvider.hpp"
#include "impl/PocketTTSImpl.hpp"

#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace va::thirdparty::tts {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PocketVoiceSynthesisProvider::PocketVoiceSynthesisProvider(PocketVoiceSynthesisOptions options)
    : QueuedVoiceSynthesisProvider("PocketTTS", Delivery::Thread),
      _impl(std::make_shared<PocketTTSImpl>(options.modelDir, options.voice))
{
    auto params = _impl->getParams();
    params.lsdSteps        = std::max(1, std::min(options.lsdSteps, 10));
    params.temperature     = options.temperature;
    params.silenceDuration = std::max(0.0f, options.silenceDurationSeconds);
    params.decoderBatchFrames = std::max(1, options.decoderBatchFrames);
    _impl->setParams(params);
    setPreBufferedSamplesMS(std::max(0, options.prebufferMs));
}

PocketVoiceSynthesisProvider::~PocketVoiceSynthesisProvider() {
    stopVoiceSynthesisProvider();
}

bool PocketVoiceSynthesisProvider::loadEngine() {
    if (!_impl->initialize()) {
        spdlog::error("[PocketTTS] Initialization failed.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------

bool PocketVoiceSynthesisProvider::setVoice(const std::string& voiceName) {
    auto names = _impl->getVoiceNames();
    bool found = std::any_of(names.begin(), names.end(),
        [&](const std::string& n) { return n == voiceName; });
    auto p = _impl->getParams();
    p.voiceName = voiceName;
    _impl->setParams(p);
    if (!found) spdlog::warn("[PocketTTS] Voice '{}' not in voices.bin; will fall back.", voiceName);
    return found;
}

std::string PocketVoiceSynthesisProvider::getVoice() const {
    return _impl->getParams().voiceName;
}

std::vector<std::string> PocketVoiceSynthesisProvider::getAvailableVoices() const {
    return _impl->getVoiceNames();
}

// ---------------------------------------------------------------------------
// synthesizeOne - runs ONNX inference, hands audio chunks to the delivery thread
// ---------------------------------------------------------------------------

bool PocketVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
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
    VASDK_CTRACK_NAME("PocketTTS::synthesize");
    // On Android the CPU is throttled between utterances: the first latent frames can take longer
    // than the audio they make, so the base class pre-buffers (setPreBufferedSamplesMS()).
    _impl->synthesize(text, cancelFlag(), [this](std::vector<float> chunk) {
        handoffAudio(std::move(chunk), modelSampleRate());
    });
    return finishHandoff();
}

// ---------------------------------------------------------------------------
// Text normalisation (delegates to PocketTTSImpl static helpers, exposed here
// to allow pre-processing before enqueuing so the worker thread doesn't
// stall on normalization).
// ---------------------------------------------------------------------------

/*static*/ std::string PocketVoiceSynthesisProvider::normalizeText(const std::string& text) {
    return PocketTTSImpl::normalizeText(text);
}

} // namespace va::thirdparty::tts
