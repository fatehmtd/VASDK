// The catalog's audio DSP (rnnoise) and speaker embedding (pyannote), picked by config id as a
// session picks them: each created through the registry and run on audio, then a C API session
// whose config names both, its pipeline checked. Exits 77 (skipped) without pyannote's model.
//   dsp_speaker_test <models dir>

#include "ProviderCatalog/ProviderCatalog.h"
#include "ProviderCatalog/ProviderCatalogC.h"

#include <VASDK/GenericAudioDSPProvider.h>
#include <VASDK/SpeakerEmbeddingProvider.h>
#include <VASDK/c/vasdk.h>
#include <VASDK/engine/ProviderRegistry.h>
#include "vasdk/engine/engine_config.pb.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

vasdk::engine::ProviderSpec spec(const char* id) {
    vasdk::engine::ProviderSpec provider;
    provider.set_id(id);
    return provider;
}

// The C API's minimal providers, so the session needs no other model.
float silent(void*, const short*, size_t) { return 0.0f; }
void heardNothing(void*, const short*, size_t) {}
int saidNothing(void*, const char*) { return 1; }
void* instance(vasdk_sink*, int, void*) { return reinterpret_cast<void*>(1); }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: dsp_speaker_test <models dir>\n");
        return 2;
    }
    const std::filesystem::path models = argv[1];
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);

    va::audio::AudioConfig audio{};   // 16 kHz mono 16-bit, the pipeline's default
    audio.sampleRate = 16000;
    audio.format = va::audio::AudioFormat::SHORT;
    audio.channels = 1;
    const va::engine::ProviderContext context{models.string(), audio, 1600, nullptr};

    // A second of noise
    std::mt19937 random(7);
    std::normal_distribution<float> noise(0.0f, 3000.0f);
    std::vector<short> pcm(16000);
    for (auto& sample : pcm) sample = static_cast<short>(std::clamp(noise(random), -32768.0f, 32767.0f));

    bool haveDsp = false;
    for (const auto& id : registry.ids<va::audio::GenericAudioDSPProvider>()) haveDsp |= id == "rnnoise";
    if (haveDsp) {
        std::string error;
        auto dsp = registry.create<va::audio::GenericAudioDSPProvider>(spec("rnnoise"), context, &error);
        check(dsp && dsp->initialize(audio), "rnnoise starts: " + error);
        if (dsp) {
            std::vector<short> cleaned(pcm.size());
            const size_t written = dsp->processAudio(reinterpret_cast<const char*>(pcm.data()), reinterpret_cast<char*>(cleaned.data()), pcm.size());
            double before = 0, after = 0;
            for (size_t i = 0; i < pcm.size(); ++i) {
                before += std::abs(pcm[i]);
                after += std::abs(cleaned[i]);
            }
            check(written == pcm.size(), "rnnoise writes every sample");
            check(after < before, "rnnoise takes noise out");
        }
    }

    const bool havePyannote = std::filesystem::exists(models / "pyannote_embedding.onnx");
    if (havePyannote) {
        std::string error;
        auto speaker = registry.create<va::audio::SpeakerEmbeddingProvider>(spec("pyannote"), context, &error);
        check(speaker != nullptr, "pyannote starts: " + error);
        if (speaker) {
            std::vector<float> samples(pcm.size() * 2);
            for (size_t i = 0; i < samples.size(); ++i) samples[i] = pcm[i % pcm.size()] / 32768.0f;
            check(!speaker->computeEmbedding(samples.data(), samples.size()).empty(), "pyannote gives a voice print");
        }
    }

    // A session that names both: the catalog's providers, the app's for the rest
    if (haveDsp && havePyannote) {
        vasdk_providers* providers = vasdk_providers_create();
        vasdk_providers_add_all(providers);
        vasdk_providers_add_vad(providers, "app", instance, nullptr, silent, nullptr);
        vasdk_providers_add_stt(providers, "app", instance, nullptr, nullptr, heardNothing, nullptr, nullptr, nullptr);
        vasdk_providers_add_tts(providers, "app", instance, nullptr, saidNothing, nullptr, nullptr);
        const auto config = std::filesystem::temp_directory_path() / "vasdk_dsp_speaker_test.txtpb";
        std::ofstream(config) << "models_dir: \"" << models.generic_string() << "\"\n"
                              << R"(providers { vad { id: "app" } stt { id: "app" } tts { id: "app" } )"
                              << R"(audio_dsp { id: "rnnoise" } speaker_embedding { id: "pyannote" } })"
                              << "\nspeaker_identification { threshold: 0.5 }\n";
        char* error = nullptr;
        vasdk_session* session = vasdk_session_create(config.string().c_str(), providers, nullptr, nullptr, &error);
        check(session != nullptr, std::string("a session with rnnoise and pyannote: ") + (error ? error : ""));
        vasdk_free(error);
        if (session) {
            check(vasdk_session_start(session, nullptr) != 0, "it starts");
            vasdk_session_push_audio(session, pcm.data(), pcm.size());
            char* described = vasdk_session_describe(session);
            const std::string pipeline = described ? described : "";
            vasdk_free(described);
            check(pipeline.find("AudioDSPProcessor") != std::string::npos && pipeline.find("SpeakerIdentificationProcessor") != std::string::npos,
                  "its pipeline has both: " + pipeline);
            vasdk_session_free(session);
        }
        vasdk_providers_free(providers);
        std::filesystem::remove(config);
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    if (!havePyannote) {
        std::printf("skipped: no %s\n", (models / "pyannote_embedding.onnx").string().c_str());
        return 77;
    }
    std::puts(haveDsp ? "rnnoise and pyannote ok" : "pyannote ok (rnnoise not built)");
    return 0;
}
