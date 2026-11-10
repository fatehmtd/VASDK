// Noise suppression: English speech clips mixed with pink noise and babble at 0, 5 and 10 dB SNR,
// passed through the provider in 100 ms frames, and transcribed by the judge before and after:
// what the denoiser does to recognition. Noise-only clips measure how much it takes out.

#include "Metrics.h"
#include "Stages.h"

#include <VASDK/GenericAudioDSPProvider.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/WavFile.h>
#include <VASDK/metrics/TranscriptScore.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace va::thirdparty::evaluation {

namespace {

constexpr double kSnrs[] = {0.0, 5.0, 10.0};
constexpr const char* kNoises[] = {"pink", "babble"};

std::vector<short> denoised(va::audio::GenericAudioDSPProvider& dsp, const std::vector<short>& pcm, size_t frame) {
    std::vector<short> out(pcm.size(), 0);
    for (size_t at = 0; at < pcm.size(); at += frame) {
        const size_t count = std::min(frame, pcm.size() - at);
        dsp.processAudio(reinterpret_cast<const char*>(pcm.data() + at), reinterpret_cast<char*>(out.data() + at), count);
    }
    return out;
}

std::string snrName(double snr) { return std::to_string(static_cast<int>(snr)) + "db"; }

} // namespace

StageResult evaluateDenoise(const StageContext& stage, const std::string& provider, Report& report) {
    if (!isBuilt<va::audio::GenericAudioDSPProvider>(*stage.registry, provider)) return {false, "denoise " + provider + " isn't built"};
    if (stage.judge.empty()) return {false, "no STT is built to judge denoised speech"};
    std::string reason;
    auto dsp = createProvider<va::audio::GenericAudioDSPProvider>(stage, provider, &reason);
    if (!dsp || !dsp->initialize(stage.context.audio)) return {false, "denoise " + provider + " doesn't start: " + reason};
    auto judge = createProvider<va::audio::GenericTranscriptionProvider>(stage, stage.judge, &reason);
    if (!judge || !judge->initializeTranscriptionProvider()) return {false, "the judge, stt " + stage.judge + ", doesn't start: " + reason};
    judge->setAudioConfiguration(stage.context.audio);
    judge->startTranscriptionProvider();
    const auto frame = static_cast<size_t>(stage.context.frameSize);

    std::vector<std::pair<std::string, std::vector<float>>> noises;
    for (const auto* clip : stage.data->clips(vasdk::eval::NOISE, stage.tier)) {
        for (const char* wanted : kNoises) {
            if (clip->noise() != wanted) continue;
            std::vector<short> pcm;
            if (!stage.data->audio(*clip, pcm, &reason)) return {false, reason};
            noises.push_back({wanted, va::audio::fromPcm16(pcm.data(), pcm.size())});
        }
    }
    if (noises.empty()) return {false, "no pink or babble noise in the set"};

    // Speech: the English clips of the tier
    const auto speech = stage.data->clips(vasdk::eval::SPEECH, stage.tier, "en-US");
    std::vector<std::vector<float>> clean;
    for (const auto* clip : speech) {
        std::vector<short> pcm;
        if (!stage.data->audio(*clip, pcm, &reason)) return {false, reason};
        clean.push_back(va::audio::fromPcm16(pcm.data(), pcm.size()));
    }
    if (clean.empty()) return {false, "no English speech clips in the set"};
    const auto clips = static_cast<int>(clean.size());
    const std::string judged = "judge " + stage.judge;
    {
        // Clean speech, as it is and through the denoiser: what it does where there is nothing to take out
        va::metrics::EditCounts plain, through;
        for (size_t i = 0; i < clean.size(); ++i) {
            const auto pcm = va::audio::toPcm16(clean[i].data(), clean[i].size());
            std::string error;
            plain += va::metrics::scoreWords(speech[i]->text(), transcribeOnce(*judge, pcm, &error));
            const auto out = denoised(*dsp, pcm, frame);
            through += va::metrics::scoreWords(speech[i]->text(), transcribeOnce(*judge, out, &error));
            if (!stage.outputFolder.empty() && i < 3) {
                const auto folder = std::filesystem::path(stage.outputFolder) / "denoise" / provider;
                std::filesystem::create_directories(folder);
                const auto samples = va::audio::fromPcm16(out.data(), out.size());
                va::audio::writeWav((folder / (speech[i]->id() + "_clean.wav")).string(), samples.data(), samples.size(), kSampleRate, 1);
            }
        }
        report.add({"denoise", provider, judged, "en-US", "wer_clean", plain.errorRate(), false, false, Unit::Rate, clips});
        report.add({"denoise", provider, judged, "en-US", "wer_denoised_clean", through.errorRate(), false, true, Unit::Rate, clips});
    }
    for (const double snr : kSnrs) {
        va::metrics::EditCounts noisy, cleaned;
        for (size_t i = 0; i < clean.size(); ++i) {
            const auto& noise = noises[i % noises.size()];
            const auto mixed = va::audio::toPcm16(mixAtSnr(clean[i], noise.second, snr).data(), clean[i].size());
            std::string error;
            const auto heardNoisy = transcribeOnce(*judge, mixed, &error);
            const auto heardCleaned = transcribeOnce(*judge, denoised(*dsp, mixed, frame), &error);
            noisy += va::metrics::scoreWords(speech[i]->text(), heardNoisy);
            const auto score = va::metrics::scoreWords(speech[i]->text(), heardCleaned);
            cleaned += score;
            if (stage.verbose)
                std::printf("denoise %s %s %s %s: noisy \"%s\", denoised \"%s\"\n", provider.c_str(), speech[i]->id().c_str(), noise.first.c_str(),
                            snrName(snr).c_str(), heardNoisy.c_str(), heardCleaned.c_str());
        }
        report.add({"denoise", provider, judged, "en-US", "wer_noisy_" + snrName(snr), noisy.errorRate(), false, false, Unit::Rate, clips});
        report.add({"denoise", provider, judged, "en-US", "wer_denoised_" + snrName(snr), cleaned.errorRate(), false, true, Unit::Rate, clips});
    }
    // How much of the noise alone it takes out, each from a fresh denoiser (it adapts to what it heard)
    std::vector<double> attenuation;
    for (const auto& noise : noises) {
        auto fresh = createProvider<va::audio::GenericAudioDSPProvider>(stage, provider, &reason);
        if (!fresh || !fresh->initialize(stage.context.audio)) return {false, "denoise " + provider + " doesn't start again: " + reason};
        const auto pcm = va::audio::toPcm16(noise.second.data(), noise.second.size());
        const auto out = denoised(*fresh, pcm, frame);
        fresh->release();
        const double before = rms(noise.second), after = rms(va::audio::fromPcm16(out.data(), out.size()));
        const double decibels = after > 0 ? 20 * std::log10(before / after) : 60.0;
        attenuation.push_back(decibels);
        report.note("denoise", provider, noise.first + " noise alone: " + formatValue(decibels, Unit::Ratio) + " dB quieter");
    }
    report.add({"denoise", provider, "", "", "noise_attenuation_db", mean(attenuation), true, true, Unit::Ratio,
                static_cast<int>(noises.size())});
    judge->stopTranscriptionProvider();
    judge->releaseTranscriptionProvider();
    dsp->release();
    return {true, {}};
}

} // namespace va::thirdparty::evaluation
