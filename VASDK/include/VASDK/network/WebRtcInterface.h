#pragma once

#include "NetworkError.h"
#include <cstddef>
#include <cstdint>
#include <functional>

namespace va {
namespace network {

/**
 * @brief Media seam between WebRtcTransport and a WebRTC stack.
 *
 * Deals only in mono 16-bit PCM: codecs, RTP, jitter handling and signaling live in the
 * provider. That keeps WebRtcTransport the same whether the peer is a browser reached through
 * a direct PeerConnection (libdatachannel) or a room on a platform such as LiveKit or Daily,
 * whose SDKs hand out decoded PCM. Signaling is provider-specific (an SDP offer/answer, a room
 * token, ...) and is exposed by each provider, not here.
 */
class WebRtcInterface {
public:
    virtual ~WebRtcInterface() = default;

    /**
     * @brief Prepare the media path. Called by WebRtcTransport::initialize().
     * @param inputSampleRate  Rate onAudioReceived must deliver (the pipeline's capture rate)
     * @param outputSampleRate Rate of the PCM passed to sendAudio (the TTS output rate)
     * @return false if the provider cannot serve these rates
     */
    virtual bool open(int inputSampleRate, int outputSampleRate) = 0;

    /// Drop any peer and release the media path.
    virtual void close() = 0;

    /// True while a remote peer is connected and media can flow.
    virtual bool isConnected() const = 0;

    /**
     * @brief Send one 20 ms chunk of PCM to the peer. WebRtcTransport paces calls in real time.
     * @return false if it could not be sent (no peer, encoder error)
     */
    virtual bool sendAudio(const int16_t* pcm, size_t samples) = 0;

    // Callbacks - set by WebRtcTransport before open(); invoked on provider threads.
    std::function<void(const int16_t* pcm, size_t samples)> onAudioReceived;
    std::function<void()>                                   onConnect;
    std::function<void()>                                   onDisconnect;
    std::function<void(NetworkError)>                       onError;
};

} // namespace network
} // namespace va
