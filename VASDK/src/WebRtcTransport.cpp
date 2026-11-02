#include "VASDK/WebRtcTransport.h"
#include "AudioFrameUtil.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>

#ifdef VASDK_HAS_JSON
#include <nlohmann/json.hpp>
#endif

// Undefine Windows ERROR macro to avoid conflict with TransportState::ERROR
#ifdef ERROR
#undef ERROR
#endif

namespace va {
namespace transport {

namespace {
audio::AudioConfig defaultConfig() {
    audio::AudioConfig c;
    c.sampleRate = 16000;
    c.channels = 1;
    c.format = audio::AudioFormat::SHORT;
    return c;
}

bool isMonoS16(const audio::AudioConfig& c) {
    return c.channels == 1 && c.format == audio::AudioFormat::SHORT && c.sampleRate > 0;
}

/// The pipeline frames a peer's UI can show, as {"type": ...} JSON; empty for any other frame.
std::string toEventJson(const std::shared_ptr<Frame>& frame) {
#ifdef VASDK_HAS_JSON
    nlohmann::json j;
    switch (frame->getType()) {
    case FrameType::AGENT_STATE:
        if (auto f = std::dynamic_pointer_cast<AgentStateFrame>(frame))
            j = {{"type", "agent_state"}, {"state", toString(f->currentState)},
                 {"from", toString(f->previousState)}, {"reason", toString(f->transition)}};
        break;
    case FrameType::VAD_STATE:
        if (auto f = std::dynamic_pointer_cast<VADStateFrame>(frame))
            j = {{"type", "vad"}, {"state", toString(f->state)}, {"confidence", f->confidence}};
        break;
    case FrameType::TURN_STATE:
        if (auto f = std::dynamic_pointer_cast<TurnStateFrame>(frame)) {
            static const char* actors[] = {"USER", "SYSTEM", "OTHER"};
            static const char* kinds[] = {"NORMAL", "INTERRUPT", "OVERLAP"};
            j = {{"type", "turn"}, {"actor", actors[static_cast<int>(f->actor)]},
                 {"state", f->state == TurnStateFrame::TurnState::START ? "START" : "END"},
                 {"kind", kinds[static_cast<int>(f->type)]}};
        }
        break;
    case FrameType::TRANSCRIPTION:
        if (auto f = std::dynamic_pointer_cast<TranscriptionFrame>(frame))
            j = {{"type", "transcript"}, {"text", f->transcriptionResult.text}, {"final", f->isFinal},
                 {"confidence", f->transcriptionResult.confidence}};
        break;
    case FrameType::TEXT:
        if (auto f = std::dynamic_pointer_cast<TextFrame>(frame))
            j = {{"type", "reply"}, {"text", f->text}, {"final", f->isFinal}};
        break;
    case FrameType::SYNTHESIS_STATE:
        if (auto f = std::dynamic_pointer_cast<SynthesisStateFrame>(frame))
            j = {{"type", "tts"}, {"state", toString(f->state)}};
        break;
    default:
        break;
    }
    if (j.is_null()) return {};
    j["t"] = frame->timestamp;
    // Model text may not be valid UTF-8; replace bad bytes rather than throw.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
#else
    (void)frame;
    return {};
#endif
}
} // namespace

WebRtcTransport::WebRtcTransport(std::shared_ptr<va::network::WebRtcInterface> peer)
    : _peer(std::move(peer))
    , _inputAudioConfig(defaultConfig())
    , _outputAudioConfig(defaultConfig())
{
}

WebRtcTransport::~WebRtcTransport() {
    stop();
    release();
}

bool WebRtcTransport::initialize() {
    if (_opened) return true;

    if (!_peer) {
        spdlog::error("[WebRtcTransport] No WebRtcInterface provider");
        return false;
    }
    if (!isMonoS16(_inputAudioConfig) || !isMonoS16(_outputAudioConfig)) {
        spdlog::error("[WebRtcTransport] Only mono 16-bit PCM is supported");
        return false;
    }
    if (_frameSizeInSamples == 0) {
        spdlog::error("[WebRtcTransport] Frame size must be at least 1 sample");
        return false;
    }

    _peer->onAudioReceived = [this](const int16_t* pcm, size_t samples) { onPeerAudio(pcm, samples); };
    _peer->onConnect       = [this] { onPeerConnected(); };
    _peer->onDisconnect    = [this] { onPeerDisconnected(); };
    _peer->onError         = [this](va::network::NetworkError err) {
        spdlog::error("[WebRtcTransport] Provider error: {}", err.message);
        incrementErrorCount();
        fireOnError(static_cast<int>(err.code), err.message);
    };

    if (!_peer->open(_inputAudioConfig.sampleRate, _outputAudioConfig.sampleRate)) {
        spdlog::error("[WebRtcTransport] Provider rejected sample rates {} / {} Hz",
                      _inputAudioConfig.sampleRate, _outputAudioConfig.sampleRate);
        release();  // drop the callbacks we just installed
        return false;
    }

    _opened = true;
    setState(TransportState::DISCONNECTED);
    spdlog::info("[WebRtcTransport] Initialized (in {} Hz, out {} Hz)",
                 _inputAudioConfig.sampleRate, _outputAudioConfig.sampleRate);
    return true;
}

bool WebRtcTransport::start() {
    if (!_opened) {
        spdlog::error("[WebRtcTransport] start() before initialize()");
        return false;
    }
    if (_running) return true;

    setState(TransportState::CONNECTING);
    _running = true;
    // onPeerConnected ignores a peer that connected before _running was set; pick it up here.
    if (_peer->isConnected()) setState(TransportState::CONNECTED);
    _senderThread = std::thread(&WebRtcTransport::senderThreadFunc, this);
    spdlog::info("[WebRtcTransport] Started - waiting for a peer");
    return true;
}

void WebRtcTransport::stop() {
    if (!_running) return;

    setState(TransportState::DISCONNECTING);
    _running = false;
    if (_senderThread.joinable()) _senderThread.join();

    setState(TransportState::DISCONNECTED);
    spdlog::info("[WebRtcTransport] Stopped");
}

void WebRtcTransport::release() {
    stop();
    if (_peer) {
        _peer->close();  // returns once no provider callback is running
        // The provider may outlive us; don't leave callbacks pointing at this transport.
        _peer->onAudioReceived = nullptr;
        _peer->onConnect = nullptr;
        _peer->onDisconnect = nullptr;
        _peer->onError = nullptr;
    }
    _opened = false;
}

bool WebRtcTransport::sendFrame(std::shared_ptr<Frame> frame) {
    if (!frame) return false;
    if (frame->getType() != FrameType::SYNTHESIS) {
        // State for the peer's UI is best effort: dropping it (no peer yet, no channel) isn't a failure.
        if (isConnected()) {
            auto json = toEventJson(frame);
            if (!json.empty()) _peer->sendEvent(json);
        }
        return true;
    }

    if (!isConnected()) {
        spdlog::debug("[WebRtcTransport] No peer connected - dropping synthesis audio");
        return false;
    }

    auto synthesisFrame = std::dynamic_pointer_cast<SynthesisFrame>(frame);
    if (!synthesisFrame) return false;

    const auto& data = synthesisFrame->audioData;
    const size_t samples = data.size() / sizeof(int16_t);
    if (samples == 0) return true;

    const auto* pcm = reinterpret_cast<const int16_t*>(data.data());
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        _outputQueue.insert(_outputQueue.end(), pcm, pcm + samples);
    }
    updateSentStats(data.size());
    return true;
}

int WebRtcTransport::getPendingOutputAudioBytes() const {
    std::lock_guard<std::mutex> lock(_outputMutex);
    return static_cast<int>(_outputQueue.size() * sizeof(int16_t));
}

void WebRtcTransport::flushOutputAudio() {
    std::lock_guard<std::mutex> lock(_outputMutex);
    // Keep ~50 ms ramped down to zero so the cut doesn't click, drop the rest.
    const size_t n = std::min(_outputQueue.size(),
                              static_cast<size_t>(_outputAudioConfig.sampleRate / 20));
    for (size_t i = 0; i < n; ++i) {
        float gain = 1.0f - static_cast<float>(i + 1) / static_cast<float>(n);
        _outputQueue[i] = static_cast<int16_t>(static_cast<float>(_outputQueue[i]) * gain);
    }
    _outputQueue.resize(n);
}

void WebRtcTransport::onPeerAudio(const int16_t* pcm, size_t samples) {
    if (!_running || !pcm || samples == 0) return;

    const size_t sampleBytes = sizeof(int16_t);
    const size_t frameBytes = _frameSizeInSamples * sampleBytes;

    std::lock_guard<std::mutex> lock(_inputMutex);
    const auto* bytes = reinterpret_cast<const char*>(pcm);
    _inputAccum.insert(_inputAccum.end(), bytes, bytes + samples * sampleBytes);

    size_t consumed = 0;
    while (_inputAccum.size() - consumed >= frameBytes) {
        auto frame = detail::writeAudioFrame(_ringBuffer.get(), _inputAccum.data() + consumed,
                                             static_cast<uint32_t>(_frameSizeInSamples),
                                             _inputAudioConfig, _absoluteOffset,
                                             _sequenceCounter++, "WebRtcTransport");
        enqueueReceivedFrame(frame);
        updateReceivedStats(frameBytes);
        consumed += frameBytes;
    }
    _inputAccum.erase(_inputAccum.begin(), _inputAccum.begin() + consumed);
}

void WebRtcTransport::onPeerConnected() {
    spdlog::info("[WebRtcTransport] Peer connected");
    if (_running) setState(TransportState::CONNECTED);  // setState fires onConnected
}

void WebRtcTransport::onPeerDisconnected() {
    spdlog::info("[WebRtcTransport] Peer disconnected");
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        _outputQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        _inputAccum.clear();
    }
    if (!_running) return;
    // CONNECTED -> CONNECTING (ready for the next peer) isn't a transition setState reports
    setState(TransportState::CONNECTING);
    _connectedGauge.set(0.0);
    fireOnDisconnected();
}

void WebRtcTransport::senderThreadFunc() {
    using clock = std::chrono::steady_clock;
    constexpr auto kTick = std::chrono::milliseconds(20);
    const size_t chunk = static_cast<size_t>(_outputAudioConfig.sampleRate) / 50;
    std::vector<int16_t> buf(chunk);

    auto next = clock::now();
    while (_running) {
        next += kTick;
        std::this_thread::sleep_until(next);
        // After a stall (debugger, suspended laptop) resync instead of bursting to catch up.
        if (clock::now() - next > 5 * kTick) next = clock::now();

        if (!_peer->isConnected()) continue;

        size_t n;
        {
            std::lock_guard<std::mutex> lock(_outputMutex);
            n = std::min(chunk, _outputQueue.size());
            std::copy_n(_outputQueue.begin(), n, buf.begin());
            _outputQueue.erase(_outputQueue.begin(), _outputQueue.begin() + n);
        }
        std::fill(buf.begin() + n, buf.end(), 0);  // silence keeps the RTP clock running

        if (!_peer->sendAudio(buf.data(), chunk)) incrementDroppedFrameCount();
    }
}

} // namespace transport
} // namespace va
