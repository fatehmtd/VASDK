// LibDataChannelWebRtcProvider over a real loopback PeerConnection: a libdatachannel offerer sends a
// 16 kHz Opus tone, the provider answers and must deliver it back as PCM of the right level; audio
// the provider sends must arrive at the offerer as Opus RTP. Also covers RtpReorderBuffer ordering,
// loss reporting, sequence wrap and late-packet dropping.

#include "LibDataChannelWebRtc/LibDataChannelWebRtcProvider.h"
#include "LibDataChannelWebRtc/RtpPayload.h"
#include "LibDataChannelWebRtc/RtpReorderBuffer.h"

#include <opus.h>
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

void reorderTests() {
    using va::thirdparty::RtpReorderBuffer;
    auto run = [](std::vector<uint16_t> seqs, std::vector<uint16_t>& out, int& lost) {
        RtpReorderBuffer buf(3);
        for (uint16_t s : seqs)
            buf.push(s, RtpReorderBuffer::Payload{std::byte(s & 0xFF), std::byte(s >> 8)},
                     [&](const RtpReorderBuffer::Payload& p) {
                         out.push_back(static_cast<uint16_t>(std::to_integer<int>(p[0]) |
                                                             (std::to_integer<int>(p[1]) << 8)));
                     },
                     [&](int n) { lost += n; });
    };
    std::vector<uint16_t> out;
    int lost = 0;
    run({10, 12, 11, 13}, out, lost);
    check(out == std::vector<uint16_t>({10, 11, 12, 13}) && lost == 0, "reorder: swap restored");

    out.clear(); lost = 0;
    run({1, 3, 4, 5, 6}, out, lost);
    check(out == std::vector<uint16_t>({1, 3, 4, 5, 6}) && lost == 1, "reorder: gap reported once hold is full");

    out.clear(); lost = 0;
    run({65534, 0, 65535, 1}, out, lost);
    check(out == std::vector<uint16_t>({65534, 65535, 0, 1}) && lost == 0, "reorder: 16-bit wrap");

    out.clear(); lost = 0;
    run({5, 6, 5, 4}, out, lost);
    check(out == std::vector<uint16_t>({5, 6}), "reorder: late and duplicate dropped");

    out.clear(); lost = 0;
    run({20000, 20001, 100, 101}, out, lost);
    check(out == std::vector<uint16_t>({20000, 20001, 100, 101}), "reorder: backwards restart resyncs");
}

void rtpPayloadTests() {
    using va::thirdparty::rtpPayloadRange;
    auto range = [](std::vector<int> bytes, size_t& b, size_t& e) {
        std::vector<std::byte> v;
        for (int x : bytes) v.push_back(std::byte(x));
        return rtpPayloadRange(v.data(), v.size(), b, e);
    };
    std::vector<int> plain(12, 0); plain[0] = 0x80; plain.insert(plain.end(), {1, 2, 3});
    size_t b = 0, e = 0;
    check(range(plain, b, e) && b == 12 && e == 15, "rtp: plain payload");

    auto padded = plain; padded[0] |= 0x20; padded.back() = 2;
    check(range(padded, b, e) && b == 12 && e == 13, "rtp: padding stripped");

    auto overPad = std::vector<int>(80, 0); overPad[0] = 0xA0; overPad.back() = 200;
    check(!range(overPad, b, e), "rtp: padding longer than packet rejected");

    auto csrc = std::vector<int>(12, 0); csrc[0] = 0x8F;  // claims 15 CSRCs
    check(!range(csrc, b, e), "rtp: CSRCs past the end rejected");

    auto ext = std::vector<int>(12, 0); ext[0] = 0x90;  // extension bit, no extension header
    check(!range(ext, b, e), "rtp: missing extension header rejected");

    auto bigExt = std::vector<int>(20, 0); bigExt[0] = 0x90; bigExt[14] = 0xFF; bigExt[15] = 0xFF;
    check(!range(bigExt, b, e), "rtp: extension longer than packet rejected");

    check(!range({0x80, 0, 0}, b, e), "rtp: short packet rejected");
}

std::vector<int16_t> tone(size_t samples, size_t offset) {
    std::vector<int16_t> v(samples);
    const double pi = std::acos(-1.0);   // M_PI isn't standard (MSVC needs _USE_MATH_DEFINES)
    for (size_t i = 0; i < samples; ++i)
        v[i] = static_cast<int16_t>(8000 * std::sin(2 * pi * 440 * double(offset + i) / 16000));
    return v;
}

double rms(const std::vector<int16_t>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return v.size() > from ? std::sqrt(s / double(v.size() - from)) : 0;
}

void loopbackTest() {
    va::thirdparty::LibDataChannelWebRtcProvider provider(va::thirdparty::LibDataChannelWebRtcOptions{{}});  // host candidates only
    std::mutex m;
    std::vector<int16_t> received;
    std::atomic<int> connects{0};
    provider.onAudioReceived = [&](const int16_t* pcm, size_t n) {
        std::lock_guard<std::mutex> l(m);
        received.insert(received.end(), pcm, pcm + n);
    };
    provider.onConnect = [&] { ++connects; };
    check(provider.open(16000, 16000), "provider opens at 16 kHz");
    check(!provider.open(44100, 16000), "provider rejects a non-Opus rate");
    check(provider.open(16000, 16000), "provider reopens");

    // Offerer: what a browser does, with libdatachannel.
    auto pc = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
    const uint32_t ssrc = 4242;
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(111);
    audio.addSSRC(ssrc, "test");
    auto track = pc->addTrack(audio);
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "test", 111,
                                                                   rtc::OpusRtpPacketizer::DefaultClockRate);
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track->setMediaHandler(packetizer);
    std::atomic<int> rtpFromProvider{0};
    track->onMessage([&](rtc::binary msg) {
        if (msg.size() > 12 && (std::to_integer<int>(msg[1]) & 0x7F) == 111) ++rtpFromProvider;
    }, nullptr);
    // Like the demo page: an events channel created before the offer.
    auto events = pc->createDataChannel("events");
    std::atomic<bool> gotEvent{false};
    events->onMessage([&](rtc::message_variant msg) {
        if (auto* s = std::get_if<std::string>(&msg)) gotEvent = *s == "{\"type\":\"test\"}";
    });

    auto gathered = std::make_shared<std::promise<void>>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    pc->onGatheringStateChange([gathered, done](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete && !done->exchange(true)) gathered->set_value();
    });
    pc->setLocalDescription();
    gathered->get_future().wait_for(10s);

    std::string answer = provider.handleOffer(std::string(*pc->localDescription()));
    check(!answer.empty(), "provider answers the offer");
    if (answer.empty()) return;
    pc->setRemoteDescription(rtc::Description(answer, "answer"));

    for (int i = 0; i < 100 && !(provider.isConnected() && track->isOpen()); ++i) std::this_thread::sleep_for(50ms);
    check(provider.isConnected() && connects == 1, "peer connects, onConnect fired once");
    check(track->isOpen(), "offerer track opens");
    check(provider.handleOffer(std::string(*pc->localDescription())).empty(), "second offer refused while connected");

    // Provider -> offerer event; the channel opens shortly after the peer connects.
    bool sent = false;
    for (int i = 0; i < 60 && !sent; ++i) {
        sent = provider.sendEvent("{\"type\":\"test\"}");
        if (!sent) std::this_thread::sleep_for(50ms);
    }
    for (int i = 0; i < 40 && !gotEvent; ++i) std::this_thread::sleep_for(25ms);
    check(sent && gotEvent, "event reaches the offerer on its data channel");

    // Offerer -> provider: 1 s of tone as 20 ms Opus packets.
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    std::vector<unsigned char> pkt(1500);
    for (int i = 0; i < 50; ++i) {
        auto pcm = tone(320, size_t(i) * 320);
        int n = opus_encode(enc, pcm.data(), 320, pkt.data(), int(pkt.size()));
        auto* b = reinterpret_cast<const std::byte*>(pkt.data());
        track->sendFrame(rtc::binary(b, b + n), rtc::FrameInfo(uint32_t(i) * 960));
        std::this_thread::sleep_for(5ms);
    }
    opus_encoder_destroy(enc);

    // Provider -> offerer.
    for (int i = 0; i < 20; ++i) provider.sendAudio(tone(320, size_t(i) * 320).data(), 320);

    std::this_thread::sleep_for(500ms);
    {
        std::lock_guard<std::mutex> l(m);
        check(received.size() >= 45 * 320, "provider delivered ~1 s of PCM");
        double level = rms(received, received.size() / 2);  // past the codec warm-up
        check(level > 8000 / std::sqrt(2.0) * 0.6 && level < 8000 / std::sqrt(2.0) * 1.4,
              "decoded tone keeps its level");
        std::printf("received %zu samples, rms %.0f\n", received.size(), level);
    }
    check(rtpFromProvider >= 15, "provider audio reached the offerer as Opus RTP");
    std::printf("offerer got %d RTP packets from provider\n", rtpFromProvider.load());

    // The offerer closing is seen via SCTP (data channel); without one the 5 s media watchdog catches it.
    pc->close();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200 && provider.isConnected(); ++i) std::this_thread::sleep_for(50ms);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(!provider.isConnected() && ms < 7000, "provider sees the peer leave within the media timeout");
    std::printf("peer leave detected after %lld ms\n", static_cast<long long>(ms));
    provider.close();
}

} // namespace

int main() {
    reorderTests();
    rtpPayloadTests();
    loopbackTest();
    std::printf("webrtc provider checks done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
