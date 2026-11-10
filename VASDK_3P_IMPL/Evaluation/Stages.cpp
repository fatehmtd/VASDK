#include "Stages.h"

#include <VASDK/GenericAudioDSPProvider.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/SpeakerEmbeddingProvider.h>
#include <VASDK/TurnDetectionProvider.h>
#include <VASDK/VADProvider.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace va::thirdparty::evaluation {

namespace {

// The catalog's local providers per stage; every other id of a stage is a cloud service.
struct LocalIds {
    const char* stage;
    const char* ids[4];
};
constexpr LocalIds kLocal[] = {
    {"stt", {"whistle", "moonshine", "parakeet", nullptr}},
    {"tts", {"pocket", "phonon", "supertonic", "moss"}},
    {"vad", {"silero", nullptr, nullptr, nullptr}},
    {"turn", {"smartturn-v3", nullptr, nullptr, nullptr}},
    {"speaker", {"pyannote", nullptr, nullptr, nullptr}},
    {"denoise", {"rnnoise", nullptr, nullptr, nullptr}},
};

// Judges, most accurate first
constexpr const char* kJudges[] = {"parakeet", "whistle", "moonshine"};

template <class Interface>
std::vector<std::string> builtLocal(const va::engine::ProviderRegistry& registry, const std::string& stage) {
    std::vector<std::string> found;
    for (const auto& local : kLocal) {
        if (stage != local.stage) continue;
        for (const char* id : local.ids)
            if (id && isBuilt<Interface>(registry, id)) found.push_back(id);
    }
    return found;
}

} // namespace

std::vector<std::string> localProviders(const va::engine::ProviderRegistry& registry, const std::string& stage) {
    if (stage == "stt") return builtLocal<va::audio::GenericTranscriptionProvider>(registry, stage);
    if (stage == "tts") return builtLocal<va::audio::GenericVoiceSynthesisProvider>(registry, stage);
    if (stage == "vad") return builtLocal<va::audio::VADProvider>(registry, stage);
    if (stage == "turn") return builtLocal<va::audio::TurnDetectionProvider>(registry, stage);
    if (stage == "speaker") return builtLocal<va::audio::SpeakerEmbeddingProvider>(registry, stage);
    if (stage == "denoise") return builtLocal<va::audio::GenericAudioDSPProvider>(registry, stage);
    return {};
}

std::string judgeFor(const va::engine::ProviderRegistry& registry, const std::string& wanted) {
    if (!wanted.empty()) return isBuilt<va::audio::GenericTranscriptionProvider>(registry, wanted) ? wanted : std::string();
    for (const char* id : kJudges)
        if (isBuilt<va::audio::GenericTranscriptionProvider>(registry, id)) return id;
    return {};
}

std::string transcribeOnce(va::audio::GenericTranscriptionProvider& recognizer, const std::vector<short>& pcm, std::string* error) {
    if (pcm.empty()) return {};
    try {
        return recognizer.writeFullAudioData(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(short)).text;
    } catch (const std::exception& thrown) {
        if (error) *error = thrown.what();
        return {};
    }
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace va::thirdparty::evaluation
