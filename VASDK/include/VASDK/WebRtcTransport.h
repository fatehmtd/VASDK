#pragma once

#include "Transport.h"
#include "AudioRingBuffer.h"
#include "CommonAudioUtils.h"
#include <VASDK/network/WebRtcInterface.h>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace va {
namespace transport {

/**
 * @brief WebRTC audio transport - one remote peer, backed by a WebRtcInterface provider.
 *
 * Mirrors LocalTransport's contract so apps can swap one for the other without touching the
 * pipeline: inbound peer audio is written into the shared AudioRingBuffer and surfaces as
 * offset-only AudioFrames; outbound SynthesisFrames are queued and sent to the peer in real-time
 * paced 20 ms chunks (silence when idle, so RTP timing stays continuous); flushOutputAudio()
 * fades and drops queued audio for barge-in.
 *
 * The transport is CONNECTED only while a peer is connected; between peers it sits in
 * CONNECTING. Signaling (how a peer gets connected) belongs to the provider - e.g.
 * LibDataChannelWebRtcProvider::handleOffer() in VASDK_3P_IMPL.
 *
 * Audio format: mono 16-bit PCM in both directions. The provider must support the configured
 * sample rates (WebRtcInterface::open() fails otherwise).
 */
class VASDK_EXPORT WebRtcTransport : public BaseTransport {
public:
    explicit WebRtcTransport(std::shared_ptr<va::network::WebRtcInterface> peer);
    ~WebRtcTransport() override;

    std::string getName() const override { return "WebRtcTransport"; }
    std::string getDescription() const override {
        return "WebRTC audio transport for a remote peer";
    }

    bool initialize() override;
    bool start() override;
    void stop() override;
    void release() override;
    bool sendFrame(std::shared_ptr<Frame> frame) override;

    /// Capture-side format (what VAD/STT see). Set before initialize(). Default 16 kHz mono s16.
    void setInputAudioConfig(const audio::AudioConfig& config) { _inputAudioConfig = config; }
    /// Playback-side format (what TTS produces). Set before initialize(). Default 16 kHz mono s16.
    void setOutputAudioConfig(const audio::AudioConfig& config) { _outputAudioConfig = config; }
    /// Samples per emitted AudioFrame (default 2000 = 125 ms at 16 kHz).
    void setFrameSize(size_t samples) { _frameSizeInSamples = samples; }
    /// Shared ring buffer that VAD/STT read; without it frames carry offsets into nothing.
    void setRingBuffer(std::shared_ptr<AudioRingBuffer> ringBuffer) { _ringBuffer = ringBuffer; }
    std::shared_ptr<AudioRingBuffer> getRingBuffer() const { return _ringBuffer; }

    /// Bytes of TTS audio queued but not yet sent to the peer.
    int getPendingOutputAudioBytes() const;

    /// Fade out (~50 ms) and discard queued output audio (barge-in / interrupt).
    void flushOutputAudio();

    /// Turns a frame into a JSON-object event for the peer ("" = skip it).
    using EventEncoder = std::function<std::string(const Frame&)>;
    /**
     * @brief Send frames of this type to the peer as events, e.g. a provider's own frame type
     *        (VizAnim's AUDIO_FEATURES) that the transport can't decode. Overrides the built-in
     *        event for that type. Set before start().
     */
    void setEventEncoder(FrameType type, EventEncoder encoder) { _eventEncoders[type] = std::move(encoder); }

private:
    void onPeerAudio(const int16_t* pcm, size_t samples);
    void onPeerConnected();
    void onPeerDisconnected();
    void senderThreadFunc();

    std::shared_ptr<va::network::WebRtcInterface> _peer;
    audio::AudioConfig _inputAudioConfig;
    audio::AudioConfig _outputAudioConfig;
    size_t _frameSizeInSamples{2000};

    // Inbound: peer PCM accumulated up to one frame, then written to the ring buffer
    std::shared_ptr<AudioRingBuffer> _ringBuffer;
    std::vector<char> _inputAccum;
    std::mutex _inputMutex;
    uint64_t _absoluteOffset{0};
    uint64_t _sequenceCounter{0};

    // Outbound: TTS samples waiting for the paced sender
    std::deque<int16_t> _outputQueue;
    mutable std::mutex _outputMutex;
    std::thread _senderThread;
    std::atomic<bool> _running{false};
    bool _opened{false};

    std::map<FrameType, EventEncoder> _eventEncoders;
};

} // namespace transport
} // namespace va
