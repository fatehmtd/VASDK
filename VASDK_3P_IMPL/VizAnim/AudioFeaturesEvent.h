#pragma once

#include "AudioFeaturesFrame.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace va {
namespace thirdparty::audio {

/**
 * @brief Event encoder that sends AudioFeaturesFrames to a WebRTC peer, for live mic/TTS visuals.
 *
 * One event per frame, one entry in "f" per analysis window (win_ms long):
 * {"type":"audio_features","source":"mic"|"tts","win_ms":128,
 *  "f":[{"rms":..,"bands":[bass,low_mid,mid,hi_mid,treble],"centroid":..,"rolloff":..,"flux":..,
 *        "onset":0|1,"zcr":..,"pitch":Hz,"voiced":..,"chroma":[12]}]}
 *
 * @code
 * webrtc->setEventEncoder(va::transport::FrameType::AUDIO_FEATURES,
 *                         va::thirdparty::audio::audioFeaturesEventEncoder(sampleRate));
 * @endcode
 * @param sampleRate rate the VizAnim processors analyse at (sets win_ms)
 */
inline std::function<std::string(const va::transport::Frame&)> audioFeaturesEventEncoder(int sampleRate) {
    const double winMs = sampleRate > 0 ? 1000.0 * VizAnimProvider::FFT_SIZE / sampleRate : 0.0;
    return [winMs](const va::transport::Frame& frame) -> std::string {
        const auto* f = dynamic_cast<const AudioFeaturesFrame*>(&frame);
        if (!f || f->featuresArray.empty()) return {};

        std::string out;
        out.reserve(64 + f->featuresArray.size() * 260);
        char buf[32];
        // JSON has no NaN/Inf.
        auto num = [&](float v) {
            std::snprintf(buf, sizeof buf, "%.3g", std::isfinite(v) ? v : 0.0f);
            out += buf;
        };
        auto field = [&](const char* key, float v) { out += ",\""; out += key; out += "\":"; num(v); };

        out += "{\"type\":\"audio_features\",\"source\":\"";
        out += f->source == AudioFeaturesSource::MIC ? "mic" : "tts";
        out += "\",\"win_ms\":";
        num(static_cast<float>(winMs));
        out += ",\"f\":[";
        for (size_t i = 0; i < f->featuresArray.size(); ++i) {
            const auto& x = f->featuresArray[i];
            if (i) out += ',';
            out += "{\"rms\":";
            num(x.rms);
            out += ",\"bands\":[";
            const float bands[] = {x.bass, x.low_mid, x.mid, x.hi_mid, x.treble};
            for (int b = 0; b < 5; ++b) { if (b) out += ','; num(bands[b]); }
            out += ']';
            field("centroid", x.centroid);
            field("rolloff", x.rolloff);
            field("flux", x.flux);
            out += x.onset ? ",\"onset\":1" : ",\"onset\":0";
            field("zcr", x.zcr);
            field("pitch", x.pitch);
            field("voiced", x.voiced);
            out += ",\"chroma\":[";
            for (int c = 0; c < VizAnimProvider::NUM_CHROMA; ++c) { if (c) out += ','; num(x.chroma[c]); }
            out += "]}";
        }
        out += "]}";
        return out;
    };
}

} // namespace thirdparty::audio
} // namespace va
