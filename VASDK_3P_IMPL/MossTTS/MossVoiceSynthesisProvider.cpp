#include "MossVoiceSynthesisProvider.hpp"
#include "impl/MossTTSImpl.hpp"

#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>

#include <algorithm>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MossVoiceSynthesisProvider::MossVoiceSynthesisProvider(
    const std::string& modelDir, const std::string& voiceName, int numThreads)
    : QueuedVoiceSynthesisProvider("MossTTS", Delivery::Thread),
      _impl(std::make_shared<MossTTSImpl>(modelDir, voiceName, numThreads))
{
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

void MossVoiceSynthesisProvider::setSilenceDuration(float seconds) {
    auto p = _impl->getParams();
    p.silenceDuration = std::max(0.0f, seconds);
    _impl->setParams(p);
}

float MossVoiceSynthesisProvider::getSilenceDuration() const {
    return _impl->getParams().silenceDuration;
}

void MossVoiceSynthesisProvider::setMaxNewFrames(int frames) {
    auto p = _impl->getParams();
    p.maxNewFrames = std::max(0, frames);
    _impl->setParams(p);
}

int MossVoiceSynthesisProvider::getMaxNewFrames() const {
    return _impl->getParams().maxNewFrames;
}

void MossVoiceSynthesisProvider::setMaxChunkTokens(int tokens) {
    auto p = _impl->getParams();
    p.maxChunkTokens = std::max(1, tokens);
    _impl->setParams(p);
}

int MossVoiceSynthesisProvider::getMaxChunkTokens() const {
    return _impl->getParams().maxChunkTokens;
}

void MossVoiceSynthesisProvider::setTextNormalization(bool enabled) {
    auto p = _impl->getParams();
    p.normalizeText = enabled;
    _impl->setParams(p);
}

bool MossVoiceSynthesisProvider::getTextNormalization() const {
    return _impl->getParams().normalizeText;
}

// ---------------------------------------------------------------------------
// synthesizeOne - runs ONNX inference, resamples, hands chunks to the delivery thread
// ---------------------------------------------------------------------------

bool MossVoiceSynthesisProvider::synthesizeOne(const std::string& text) {
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
