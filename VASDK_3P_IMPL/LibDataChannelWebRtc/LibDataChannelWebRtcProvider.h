#pragma once

#include "vasdk_libdatachannel_webrtc_export.h"
#include <VASDK/network/WebRtcInterface.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace va::thirdparty::transport {

/** Settings of LibDataChannelWebRtcProvider; every field has a default. */
struct LibDataChannelWebRtcOptions {
    /** STUN/TURN URLs, e.g. "stun:stun.l.google.com:19302". Empty: host candidates only (same machine
     *  or LAN). */
    std::vector<std::string> iceServers = {"stun:stun.l.google.com:19302"};
    /** Where a peer created from an engine config (the catalog's "libdatachannel") serves its
     *  signaling: startSignalingServer(signalingPort, signalingHost, page). */
    uint16_t signalingPort = 8080;
    std::string signalingHost = "127.0.0.1";
};

/**
 * @brief WebRtcInterface over a direct PeerConnection (libdatachannel + libopus).
 *
 * One remote peer at a time, one sendrecv Opus audio track. Signaling is a single SDP
 * offer/answer exchange (non-trickle): call handleOffer() from your own signaling, or let
 * startSignalingServer() expose it as `POST /offer` - the same shape as WHIP.
 *
 * Opus is decoded/encoded directly at the transport's rates, which must be ones Opus supports
 * (8, 12, 16, 24 or 48 kHz). Inbound packets pass a small reorder buffer; gaps are filled with
 * Opus packet-loss concealment.
 */
class VASDK_LIBDATACHANNEL_WEBRTC_EXPORT LibDataChannelWebRtcProvider : public va::network::WebRtcInterface {
public:
    explicit LibDataChannelWebRtcProvider(LibDataChannelWebRtcOptions options = {});
    ~LibDataChannelWebRtcProvider() override;

    bool open(int inputSampleRate, int outputSampleRate) override;
    void close() override;
    bool isConnected() const override;
    bool sendAudio(const int16_t* pcm, size_t samples) override;
    /// Sent on the data channel the browser opens with its offer (any label); false until it is open.
    bool sendEvent(const std::string& json) override;

    /**
     * @brief Answer a remote SDP offer, replacing any peer that is not yet connected.
     * Blocks until ICE gathering completes (up to ~10 s) so the answer carries all candidates.
     * @return answer SDP, or empty if the offer is unusable, a peer is already connected,
     *         or open() has not been called
     */
    std::string handleOffer(const std::string& offerSdp);

    /**
     * @brief Serve signaling over HTTP: `POST /offer` (body: offer SDP, reply: answer SDP; 409
     *        while a peer is connected) and, if pagePath is set, `GET /` with that HTML file.
     * There is no authentication - keep the default loopback host unless the network is trusted.
     */
    bool startSignalingServer(uint16_t port, const std::string& host = "127.0.0.1",
                              const std::string& pagePath = "");
    void stopSignalingServer();

    /**
     * @brief Serve `GET /pipeline` on the signaling server with this JSON, built per request
     *        (e.g. Pipeline::describeAsJson), so a page can show the processors and providers.
     *        Can be set before or after startSignalingServer(); 404 until it is.
     */
    void setPipelineDescription(std::function<std::string()> describePipeline);

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};

} // namespace va::thirdparty::transport
