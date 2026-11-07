#include "LibDataChannelWebRtcProvider.h"
#include "RtpPayload.h"
#include "RtpReorderBuffer.h"

#include <httplib.h>
#include <opus.h>
#include <rtc/rtc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace va::thirdparty::transport {

namespace {
constexpr int kMaxPlcPackets = 5;        // conceal at most 100 ms per gap; longer gaps stay silent
constexpr auto kGatheringTimeout = std::chrono::seconds(10);
// Browsers send RTP continuously (even muted), so silence this long means the peer is gone -
// usually much sooner than ICE consent expiry (~30 s) when no DTLS close alert arrives.
constexpr auto kMediaTimeout = std::chrono::seconds(5);

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool opusSupportsRate(int rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

struct LibDataChannelWebRtcProvider::Impl : std::enable_shared_from_this<Impl> {
    va::network::WebRtcInterface* owner = nullptr;
    std::vector<std::string> iceServers;
    int inRate = 0;
    int outRate = 0;

    // Peer (swapped by handleOffer/close; callbacks compare against it to ignore stale peers)
    std::mutex pcMutex;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    std::shared_ptr<rtc::DataChannel> events;  // opened by the browser; carries sendEvent()
    std::vector<std::string> pendingEvents;    // sent while connected, before the browser's channel opened
    std::atomic<int> payloadType{111};
    std::atomic<bool> connected{false};
    std::atomic<int64_t> lastMediaMs{0};
    std::mutex offerMutex;  // one offer at a time; httplib serves requests from a thread pool
    std::mutex cbMutex;     // held while calling onConnect/onDisconnect, so close() can wait them out

    // Watchdog: drops a connected peer whose media stopped
    std::thread watchdog;
    std::mutex watchdogMutex;
    std::condition_variable watchdogCv;
    bool watchdogStop = false;

    // Receive side: libdatachannel thread
    std::mutex decMutex;
    OpusDecoder* decoder = nullptr;
    RtpReorderBuffer reorder;
    int lastPacketSamples = 0;
    std::vector<int16_t> pcmBuf;

    // Send side: WebRtcTransport's sender thread
    std::mutex encMutex;
    OpusEncoder* encoder = nullptr;
    uint32_t rtpTimestamp = 0;
    std::vector<unsigned char> encBuf = std::vector<unsigned char>(1500);

    // Signaling
    std::unique_ptr<httplib::Server> http;
    std::mutex describePipelineMutex;
    std::function<std::string()> describePipeline;
    std::thread httpThread;

    std::shared_ptr<rtc::PeerConnection> currentPc() {
        std::lock_guard<std::mutex> lock(pcMutex);
        return pc;
    }

    void resetPeer() {
        std::shared_ptr<rtc::PeerConnection> old;
        {
            std::lock_guard<std::mutex> lock(pcMutex);
            old = std::move(pc);
            pc.reset();
            track.reset();
            rtpConfig.reset();
            events.reset();
            pendingEvents.clear();
        }
        // Close outside the lock: close() fires state callbacks synchronously.
        if (old) old->close();
        if (connected.exchange(false)) {
            std::lock_guard<std::mutex> cb(cbMutex);
            if (owner->onDisconnect) owner->onDisconnect();
        }
        std::lock_guard<std::mutex> lock(decMutex);
        reorder.reset();
        if (decoder) opus_decoder_ctl(decoder, OPUS_RESET_STATE);
    }

    void onStateChange(rtc::PeerConnection* self, rtc::PeerConnection::State state) {
        std::lock_guard<std::mutex> cb(cbMutex);
        if (currentPc().get() != self) return;  // a replaced peer winding down
        using S = rtc::PeerConnection::State;
        if (state == S::Connected) {
            spdlog::info("[LibDataChannelWebRtc] Peer connected");
            lastMediaMs = nowMs();
            if (!connected.exchange(true) && owner->onConnect) owner->onConnect();
        } else if (state == S::Failed || state == S::Closed) {
            // Disconnected is left out: ICE can recover from it, and the media watchdog catches it if not.
            spdlog::info("[LibDataChannelWebRtc] Peer gone (state {})", static_cast<int>(state));
            if (connected.exchange(false) && owner->onDisconnect) owner->onDisconnect();
        }
    }

    void onRtp(const rtc::binary& msg) {
        size_t begin = 0, end = 0;
        if (!rtpPayloadRange(msg.data(), msg.size(), begin, end)) return;
        auto* header = reinterpret_cast<const rtc::RtpHeader*>(msg.data());
        if (header->payloadType() != payloadType) return;
        lastMediaMs = nowMs();

        std::lock_guard<std::mutex> lock(decMutex);
        if (!decoder) return;
        reorder.push(header->seqNumber(), RtpReorderBuffer::Payload(msg.begin() + begin, msg.begin() + end),
            [this](const RtpReorderBuffer::Payload& p) { decode(p); },
            [this](int lost) {
                for (int i = 0; i < std::min(lost, kMaxPlcPackets) && lastPacketSamples > 0; ++i)
                    emit(opus_decode(decoder, nullptr, 0, pcmBuf.data(), lastPacketSamples, 0));
            });
    }

    void decode(const RtpReorderBuffer::Payload& p) {
        int n = opus_decode(decoder, reinterpret_cast<const unsigned char*>(p.data()),
                            static_cast<opus_int32>(p.size()), pcmBuf.data(),
                            static_cast<int>(pcmBuf.size()), 0);
        if (n > 0) lastPacketSamples = n;
        emit(n);
    }

    void watchdogLoop() {
        std::unique_lock<std::mutex> lock(watchdogMutex);
        while (!watchdogCv.wait_for(lock, std::chrono::milliseconds(500), [this] { return watchdogStop; })) {
            if (!connected) continue;
            if (nowMs() - lastMediaMs > std::chrono::duration_cast<std::chrono::milliseconds>(kMediaTimeout).count()) {
                spdlog::info("[LibDataChannelWebRtc] No media from peer for {} s - dropping it",
                             std::chrono::duration_cast<std::chrono::seconds>(kMediaTimeout).count());
                lock.unlock();
                resetPeer();
                lock.lock();
            }
        }
    }

    void emit(int samples) {
        if (samples > 0 && owner->onAudioReceived) owner->onAudioReceived(pcmBuf.data(), samples);
    }
};

LibDataChannelWebRtcProvider::LibDataChannelWebRtcProvider(LibDataChannelWebRtcOptions options)
    : _impl(std::make_shared<Impl>())
{
    _impl->owner = this;
    _impl->iceServers = std::move(options.iceServers);
}

LibDataChannelWebRtcProvider::~LibDataChannelWebRtcProvider() {
    stopSignalingServer();
    close();
}

bool LibDataChannelWebRtcProvider::open(int inputSampleRate, int outputSampleRate) {
    if (!opusSupportsRate(inputSampleRate) || !opusSupportsRate(outputSampleRate)) {
        spdlog::error("[LibDataChannelWebRtc] Opus can't run at {} / {} Hz (use 8/12/16/24/48 kHz)",
                      inputSampleRate, outputSampleRate);
        return false;
    }
    close();

    int err = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->decMutex);
        _impl->decoder = opus_decoder_create(inputSampleRate, 1, &err);
        _impl->pcmBuf.assign(static_cast<size_t>(inputSampleRate) * 120 / 1000, 0);  // max Opus packet
    }
    if (err != OPUS_OK) {
        spdlog::error("[LibDataChannelWebRtc] opus_decoder_create: {}", opus_strerror(err));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_impl->encMutex);
        _impl->encoder = opus_encoder_create(outputSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
    }
    if (err != OPUS_OK) {
        spdlog::error("[LibDataChannelWebRtc] opus_encoder_create: {}", opus_strerror(err));
        close();
        return false;
    }

    _impl->inRate = inputSampleRate;
    _impl->outRate = outputSampleRate;
    _impl->watchdogStop = false;
    _impl->watchdog = std::thread([impl = _impl.get()] { impl->watchdogLoop(); });
    return true;
}

void LibDataChannelWebRtcProvider::close() {
    std::lock_guard<std::mutex> offerLock(_impl->offerMutex);  // don't let an in-flight offer outlive us
    {
        std::lock_guard<std::mutex> lock(_impl->watchdogMutex);
        _impl->watchdogStop = true;
    }
    _impl->watchdogCv.notify_all();
    if (_impl->watchdog.joinable()) _impl->watchdog.join();
    _impl->resetPeer();
    // No peer is current now, so new state callbacks bail out; this waits for one already running.
    // The audio callback is covered by decMutex below.
    { std::lock_guard<std::mutex> cb(_impl->cbMutex); }
    {
        std::lock_guard<std::mutex> lock(_impl->decMutex);
        if (_impl->decoder) opus_decoder_destroy(_impl->decoder);
        _impl->decoder = nullptr;
    }
    std::lock_guard<std::mutex> lock(_impl->encMutex);
    if (_impl->encoder) opus_encoder_destroy(_impl->encoder);
    _impl->encoder = nullptr;
}

bool LibDataChannelWebRtcProvider::isConnected() const {
    return _impl->connected;
}

bool LibDataChannelWebRtcProvider::sendAudio(const int16_t* pcm, size_t samples) {
    std::shared_ptr<rtc::Track> track;
    {
        std::lock_guard<std::mutex> lock(_impl->pcMutex);
        track = _impl->track;
    }
    if (!track || !track->isOpen()) return false;

    std::lock_guard<std::mutex> lock(_impl->encMutex);
    if (!_impl->encoder) return false;
    int bytes = opus_encode(_impl->encoder, pcm, static_cast<int>(samples), _impl->encBuf.data(),
                            static_cast<opus_int32>(_impl->encBuf.size()));
    if (bytes < 0) {
        spdlog::warn("[LibDataChannelWebRtc] opus_encode: {}", opus_strerror(bytes));
        return false;
    }

    auto* data = reinterpret_cast<const std::byte*>(_impl->encBuf.data());
    try {
        track->sendFrame(rtc::binary(data, data + bytes), rtc::FrameInfo(_impl->rtpTimestamp));
    } catch (const std::exception& e) {
        spdlog::debug("[LibDataChannelWebRtc] send failed: {}", e.what());
        return false;
    }
    // The Opus RTP clock is always 48 kHz, whatever rate the encoder runs at.
    _impl->rtpTimestamp += static_cast<uint32_t>(samples * 48000 / _impl->outRate);
    return true;
}

namespace {

bool sendOn(rtc::DataChannel& channel, const std::string& json) {
    try {
        return channel.send(json);
    } catch (const std::exception& e) {
        spdlog::debug("[LibDataChannelWebRtc] event send failed: {}", e.what());
        return false;
    }
}

} // namespace

bool LibDataChannelWebRtcProvider::sendEvent(const std::string& json) {
    // Under the lock, so what was held for the channel goes out first and in order.
    std::lock_guard<std::mutex> lock(_impl->pcMutex);
    if (_impl->events && _impl->events->isOpen()) return sendOn(*_impl->events, json);
    // The call is up but the browser's channel isn't open yet (the greeting): held until it is.
    constexpr size_t maximumPendingEvents = 256;
    if (!_impl->pc || !_impl->connected || _impl->pendingEvents.size() >= maximumPendingEvents) return false;
    _impl->pendingEvents.push_back(json);
    return true;
}

std::string LibDataChannelWebRtcProvider::handleOffer(const std::string& offerSdp) {
    auto impl = _impl;
    std::lock_guard<std::mutex> offerLock(impl->offerMutex);
    if (impl->connected) {
        spdlog::warn("[LibDataChannelWebRtc] Rejecting offer: a peer is already connected");
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(impl->decMutex);
        if (!impl->decoder) {
            spdlog::error("[LibDataChannelWebRtc] handleOffer() before open()");
            return {};
        }
    }

    try {
        rtc::Description offer(offerSdp, rtc::Description::Type::Offer);

        // Answer on the offer's audio m-line with the payload type it chose for Opus.
        std::string mid;
        int pt = -1;
        for (int i = 0; i < offer.mediaCount() && mid.empty(); ++i) {
            auto media = offer.media(i);
            auto* m = std::get_if<rtc::Description::Media*>(&media);
            if (!m || (*m)->type() != "audio") continue;
            for (int candidate : (*m)->payloadTypes()) {
                auto* map = (*m)->rtpMap(candidate);
                if (map && lower(map->format) == "opus") { pt = candidate; break; }
            }
            if (pt >= 0) mid = (*m)->mid();
        }
        if (mid.empty()) {
            spdlog::error("[LibDataChannelWebRtc] Offer has no Opus audio m-line");
            return {};
        }

        impl->resetPeer();

        rtc::Configuration config;
        for (auto& url : impl->iceServers) config.iceServers.emplace_back(url);
        auto pc = std::make_shared<rtc::PeerConnection>(config);

        uint32_t ssrc = std::random_device{}();
        rtc::Description::Audio audio(mid, rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(pt);
        audio.addSSRC(ssrc, "vasdk-audio", "vasdk", "vasdk-audio");
        auto track = pc->addTrack(audio);  // same mid as the offer, so the answer reuses it

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "vasdk-audio", static_cast<uint8_t>(pt), rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track->setMediaHandler(packetizer);

        std::weak_ptr<Impl> weak = impl;
        rtc::PeerConnection* self = pc.get();
        track->onMessage(
            [weak](rtc::binary msg) { if (auto i = weak.lock()) i->onRtp(msg); },
            nullptr);
        pc->onStateChange([weak, self](rtc::PeerConnection::State s) {
            if (auto i = weak.lock()) i->onStateChange(self, s);
        });
        pc->onDataChannel([weak, self](std::shared_ptr<rtc::DataChannel> dc) {
            auto i = weak.lock();
            if (!i) return;
            std::lock_guard<std::mutex> lock(i->pcMutex);
            if (i->pc.get() != self) return;
            i->events = dc;
            auto flush = [weak, self] {   // what was sent before the channel opened
                auto impl = weak.lock();
                if (!impl) return;
                std::lock_guard<std::mutex> held(impl->pcMutex);
                if (impl->pc.get() != self || !impl->events || !impl->events->isOpen()) return;
                for (const auto& json : impl->pendingEvents) sendOn(*impl->events, json);
                impl->pendingEvents.clear();
            };
            if (dc->isOpen()) {
                for (const auto& json : i->pendingEvents) sendOn(*dc, json);
                i->pendingEvents.clear();
            } else {
                dc->onOpen(flush);
            }
        });

        auto gathered = std::make_shared<std::promise<void>>();
        auto done = std::make_shared<std::atomic<bool>>(false);
        pc->onGatheringStateChange([gathered, done](rtc::PeerConnection::GatheringState s) {
            if (s == rtc::PeerConnection::GatheringState::Complete && !done->exchange(true))
                gathered->set_value();
        });

        {
            std::lock_guard<std::mutex> lock(impl->pcMutex);
            impl->pc = pc;
            impl->track = track;
            impl->rtpConfig = rtpConfig;
            impl->payloadType = pt;
        }

        pc->setRemoteDescription(offer);  // auto-negotiation creates the answer and starts gathering
        if (gathered->get_future().wait_for(kGatheringTimeout) != std::future_status::ready)
            spdlog::warn("[LibDataChannelWebRtc] ICE gathering timed out; answering with what we have");

        auto answer = pc->localDescription();
        if (!answer) return {};
        spdlog::info("[LibDataChannelWebRtc] Answered offer (mid={}, opus pt={})", mid, pt);
        return std::string(*answer);
    } catch (const std::exception& e) {
        spdlog::error("[LibDataChannelWebRtc] handleOffer failed: {}", e.what());
        return {};
    }
}

void LibDataChannelWebRtcProvider::setPipelineDescription(std::function<std::string()> describePipeline) {
    std::lock_guard<std::mutex> lock(_impl->describePipelineMutex);
    _impl->describePipeline = std::move(describePipeline);
}

bool LibDataChannelWebRtcProvider::startSignalingServer(uint16_t port, const std::string& host,
                                                        const std::string& pagePath) {
    stopSignalingServer();
    auto http = std::make_unique<httplib::Server>();

    http->Post("/offer", [this](const httplib::Request& req, httplib::Response& res) {
        if (isConnected()) { res.status = 409; return; }
        auto answer = handleOffer(req.body);
        if (answer.empty()) { res.status = 400; return; }
        res.set_content(answer, "application/sdp");
    });
    http->Get("/pipeline", [impl = _impl](const httplib::Request&, httplib::Response& res) {
        std::function<std::string()> describePipeline;
        {
            std::lock_guard<std::mutex> lock(impl->describePipelineMutex);
            describePipeline = impl->describePipeline;
        }
        if (!describePipeline) { res.status = 404; return; }
        res.set_content(describePipeline(), "application/json");
    });
    if (!pagePath.empty()) {
        http->Get("/", [pagePath](const httplib::Request&, httplib::Response& res) {
            std::ifstream f(pagePath, std::ios::binary);
            if (!f) { res.status = 404; return; }
            std::stringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "text/html");
        });
    }

    if (!http->bind_to_port(host, port)) {
        spdlog::error("[LibDataChannelWebRtc] Signaling server can't bind {}:{}", host, port);
        return false;
    }
    auto* server = http.get();
    _impl->http = std::move(http);
    _impl->httpThread = std::thread([server] { server->listen_after_bind(); });
    spdlog::info("[LibDataChannelWebRtc] Signaling on http://{}:{} (POST /offer)", host, port);
    return true;
}

void LibDataChannelWebRtcProvider::stopSignalingServer() {
    if (_impl->http) _impl->http->stop();
    if (_impl->httpThread.joinable()) _impl->httpThread.join();
    _impl->http.reset();
}

} // namespace va::thirdparty::transport
