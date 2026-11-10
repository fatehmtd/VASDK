// Turn detection: each turn clip runs through the ring buffer in 100 ms frames, with the VAD's
// states where VADFrameProcessor would put them for the labelled speech: STARTING at its start,
// PAUSED on the first quiet frame after it, ENDING after maxSpeechPauseDurationMs, ENDED after
// maxSpeechEndDurationMs more (VADConfig's defaults). Only the turn model is measured, not a VAD.
//
// What the model decides is whether to end the turn during that silence, before the VAD's end: a
// finished sentence should end it (and the sooner, the snappier the reply), a sentence stopped
// mid-phrase should not (ending it cuts the user off).

#include "Metrics.h"
#include "Stages.h"

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/Frame.h>
#include <VASDK/TurnDetectionProvider.h>
#include <VASDK/VADFrameProcessor.h>

#include <cstdio>

namespace va::thirdparty::evaluation {

namespace {

constexpr long kRingSeconds = 60;
constexpr double kHesitation = 0.5;   // seconds: a pause this short inside a sentence is a hesitation

std::shared_ptr<va::transport::VADStateFrame> vadState(va::transport::VADState state, uint64_t start, uint64_t end) {
    auto frame = std::make_shared<va::transport::VADStateFrame>();
    frame->state = state;
    frame->speechStartOffset = start;
    frame->speechEndOffset = end;
    frame->confidence = 1.0f;
    return frame;
}

struct Outcome {
    bool endedEarly = false;   // during PAUSED or ENDING, before the VAD's end
    double after = 0;          // seconds from the end of the speech to the turn's end
};

Outcome runClip(va::audio::TurnDetectionProvider& turn, va::AudioRingBuffer& ring, std::vector<short> pcm, double speechStart,
                double speechEnd, size_t frame, uint64_t& absolute) {
    const va::pipeline::VADConfig vad;
    const double endingAt = speechEnd + vad.maxSpeechPauseDurationMs / 1000.0;
    const double endedAt = endingAt + vad.maxSpeechEndDurationMs / 1000.0;
    const auto needed = static_cast<size_t>((endedAt + 0.2) * kSampleRate);   // silence past the VAD's end
    if (pcm.size() < needed) pcm.resize(needed, 0);

    const long base = ring.getOffset();
    const auto startOffset = static_cast<uint64_t>(ring.normalizeOffset(base + static_cast<long>(speechStart * kSampleRate)));
    const auto endOffset = static_cast<uint64_t>(ring.normalizeOffset(base + static_cast<long>(speechEnd * kSampleRate)));
    va::transport::VADState state = va::transport::VADState::QUIET;
    for (size_t at = 0; at + frame <= pcm.size(); at += frame) {
        auto audio = std::make_shared<va::transport::AudioFrame>();
        audio->ringbufferOffset = static_cast<uint64_t>(ring.getOffset());
        ring.write(reinterpret_cast<const char*>(pcm.data() + at), 0, static_cast<int>(frame));
        audio->numSamples = static_cast<uint32_t>(frame);
        audio->sampleRate = kSampleRate;
        audio->channels = 1;
        audio->sampleSizeInBytes = sizeof(short);
        audio->format = static_cast<int>(va::audio::AudioFormat::SHORT);
        audio->absoluteOffset = absolute;
        absolute += frame;
        const double time = static_cast<double>(at + frame) / kSampleRate;   // the frame's end

        // The VAD's state for this frame, as VADFrameProcessor would emit it
        va::transport::VADState next = state;
        if (state == va::transport::VADState::QUIET && time > speechStart) next = va::transport::VADState::STARTING;
        else if (state == va::transport::VADState::STARTING) next = va::transport::VADState::ONGOING;
        if ((next == va::transport::VADState::ONGOING || next == va::transport::VADState::STARTING) && time > speechEnd)
            next = va::transport::VADState::PAUSED;
        if (next == va::transport::VADState::PAUSED && time >= endingAt) next = va::transport::VADState::ENDING;
        if (next == va::transport::VADState::ENDING && time >= endedAt) next = va::transport::VADState::ENDED;
        if (next != state) {
            state = next;
            turn.evaluate(vadState(state, startOffset, state == va::transport::VADState::STARTING ? 0 : endOffset), false);
        }
        if (state == va::transport::VADState::ENDED) break;   // the VAD ended the turn: no early end
        const auto decision = turn.evaluate(audio, false);
        if (decision.decision == va::audio::TurnDetectionProvider::Decision::TURN_END &&
            (state == va::transport::VADState::PAUSED || state == va::transport::VADState::ENDING))
            return {true, time - speechEnd};
    }
    return {};
}

} // namespace

StageResult evaluateTurn(const StageContext& stage, const std::string& provider, Report& report) {
    if (!isBuilt<va::audio::TurnDetectionProvider>(*stage.registry, provider)) return {false, "turn " + provider + " isn't built"};
    StageContext withRing = stage;
    auto ring = std::make_shared<va::AudioRingBuffer>(kRingSeconds * kSampleRate, static_cast<int>(sizeof(short)));
    withRing.context.ringBuffer = ring;
    std::string reason;
    auto turn = createProvider<va::audio::TurnDetectionProvider>(withRing, provider, &reason);
    if (!turn) return {false, "turn " + provider + " doesn't start: " + reason};
    const auto frame = static_cast<size_t>(stage.context.frameSize);

    int complete = 0, cut = 0, completeEnded = 0, cutEnded = 0, cutEndedFast = 0;
    std::vector<double> latencies;
    uint64_t absolute = 0;
    for (const auto* clip : stage.data->clips(vasdk::eval::TURN, stage.tier)) {
        if (clip->speech().empty()) continue;
        std::vector<short> pcm;
        if (!stage.data->audio(*clip, pcm, &reason)) return {false, reason};
        const double speechStart = clip->speech(0).start_ms() / 1000.0;
        const double speechEnd = clip->speech(clip->speech_size() - 1).end_ms() / 1000.0;
        const Outcome outcome = runClip(*turn, *ring, std::move(pcm), speechStart, speechEnd, frame, absolute);
        if (clip->turn_complete()) {
            ++complete;
            if (outcome.endedEarly) {
                ++completeEnded;
                latencies.push_back(outcome.after);
            }
        } else {
            ++cut;
            cutEnded += outcome.endedEarly;
            cutEndedFast += outcome.endedEarly && outcome.after <= kHesitation;
        }
        if (outcome.endedEarly != clip->turn_complete())
            report.note("turn", provider, clip->id() + (clip->turn_complete() ? ": a finished sentence, the turn waited for the VAD's end"
                                                                             : ": stopped mid-phrase, the turn ended " +
                                                                                   formatValue(outcome.after, Unit::Seconds) + " after it"));
        if (stage.verbose)
            std::printf("turn %s %s: %s\n", provider.c_str(), clip->id().c_str(),
                        outcome.endedEarly ? ("ended " + formatValue(outcome.after, Unit::Seconds) + " after the speech").c_str()
                                           : "waited for the VAD's end");
    }
    const int clips = complete + cut;
    if (clips == 0) return {false, "no turn clips in the set"};
    const int ends = completeEnded + cutEnded;
    report.add({"turn", provider, "", "en-US", "accuracy", static_cast<double>(completeEnded + cut - cutEnded) / clips, true, true, Unit::Rate, clips});
    report.add({"turn", provider, "", "en-US", "recall", complete ? static_cast<double>(completeEnded) / complete : 0, true, false, Unit::Rate, complete});
    report.add({"turn", provider, "", "en-US", "false_end", cut ? static_cast<double>(cutEnded) / cut : 0, false, false, Unit::Rate, cut});
    report.add({"turn", provider, "", "en-US", "false_end_500ms", cut ? static_cast<double>(cutEndedFast) / cut : 0, false, true, Unit::Rate, cut});
    report.add({"turn", provider, "", "en-US", "precision", ends ? static_cast<double>(completeEnded) / ends : 1.0, true, false, Unit::Rate, clips});
    report.add({"turn", provider, "", "en-US", "end_latency", median(latencies), false, false, Unit::Seconds, completeEnded});
    return {true, {}};
}

} // namespace va::thirdparty::evaluation
