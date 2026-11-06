#include "PocketVoiceSynthesisProvider.hpp"
#include "impl/PocketTTSImpl.hpp"

#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <algorithm>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PocketVoiceSynthesisProvider::PocketVoiceSynthesisProvider(
    const std::string& modelDir, const std::string& voiceName)
    : QueuedVoiceSynthesisProvider("PocketTTS", Delivery::Thread),
      _impl(std::make_shared<PocketTTSImpl>(modelDir, voiceName))
{
    setPreBufferedSamplesMS(1000);
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

void PocketVoiceSynthesisProvider::setLsdSteps(int steps) {
    auto p = _impl->getParams();
    p.lsdSteps = std::max(1, std::min(steps, 10));
    _impl->setParams(p);
}

int PocketVoiceSynthesisProvider::getLsdSteps() const {
    return _impl->getParams().lsdSteps;
}

void PocketVoiceSynthesisProvider::setSilenceDuration(float seconds) {
    auto p = _impl->getParams();
    p.silenceDuration = std::max(0.0f, seconds);
    _impl->setParams(p);
}

float PocketVoiceSynthesisProvider::getSilenceDuration() const {
    return _impl->getParams().silenceDuration;
}

void PocketVoiceSynthesisProvider::setDecoderBatchFrames(int frames) {
    auto p = _impl->getParams();
    p.decoderBatchFrames = std::max(1, frames);
    _impl->setParams(p);
}

int PocketVoiceSynthesisProvider::getDecoderBatchFrames() const {
    return _impl->getParams().decoderBatchFrames;
}

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
