#include "ProviderCatalog.h"

#include <algorithm>
#include <vasdk/providers/options.pb.h>

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/TurnDetectionProvider.h>
#include <VASDK/VADProvider.h>

#include <cstdlib>

#ifdef VASDK_HAS_SILERO_VAD
#include "SileroVAD/SileroVAD.h"
#endif
#ifdef VASDK_HAS_GRADIUMPP_STT
#include "GradiumppTranscription/GradiumppTranscriptionProvider.h"
#endif
#ifdef VASDK_HAS_SONIOXPP_STT
#include "SonioxppTranscription/SonioxppTranscriptionProvider.h"
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_STT
#include "DeepgramppTranscription/DeepgramppTranscriptionProvider.h"
#endif
#ifdef VASDK_HAS_CARTESIAPP_STT
#include "CartesiappTranscription/CartesiappTranscriptionProvider.h"
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_STT
#include "FishaudioppTranscription/FishaudioppTranscriptionProvider.h"
#endif
#ifdef VASDK_HAS_WHISTLE_STT
#include "WhistleSTT/WhistleTranscriptionProvider.hpp"
#endif
#ifdef VASDK_HAS_MOONSHINE_STT
#include "MoonshineSTT/MoonshineSttProvider.hpp"
#endif
#ifdef VASDK_HAS_PARAKEET
#include "Parakeet/ParakeetTranscriptionProvider.hpp"
#endif
#ifdef VASDK_HAS_GRADIUMPP_TTS
#include "GradiumppVoiceSynthesis/GradiumppVoiceSynthesisProvider.h"
#endif
#ifdef VASDK_HAS_SONIOXPP_TTS
#include "SonioxppVoiceSynthesis/SonioxppVoiceSynthesisProvider.h"
#endif
#ifdef VASDK_HAS_SUPERTONIC_TTS
#include "SupertonicTTS/SupertonicVoiceSynthesisProvider.hpp"
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_TTS
#include "DeepgramppVoiceSynthesis/DeepgramppVoiceSynthesisProvider.h"
#endif
#ifdef VASDK_HAS_CARTESIAPP_TTS
#include "CartesiappVoiceSynthesis/CartesiappVoiceSynthesisProvider.h"
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_TTS
#include "FishaudioppVoiceSynthesis/FishaudioppVoiceSynthesisProvider.h"
#endif
#ifdef VASDK_HAS_MOSS_TTS
#include "MossTTS/MossVoiceSynthesisProvider.hpp"
#endif
#ifdef VASDK_HAS_PHONON_TTS
#include "PhononTTS/PhononVoiceSynthesisProvider.hpp"
#endif
#ifdef VASDK_HAS_SMART_TURN_V3
#include "SmartTurnV3/SmartTurnV3Provider.h"
#endif
#ifdef VASDK_HAS_POCKET_TTS
#include "PocketTTS/PocketVoiceSynthesisProvider.hpp"
#endif

namespace va {
namespace thirdparty {

namespace {

namespace providers = ::vasdk::providers;
using engine::ProviderContext;
using Stt = audio::GenericTranscriptionProvider;
using Tts = audio::GenericVoiceSynthesisProvider;
using Vad = audio::VADProvider;
using providers::CloudOptions;
using providers::LocalSttOptions;
using providers::LocalTtsOptions;

// Config value, else the environment variable, else the fallback.
[[maybe_unused]] std::string orEnvironment(const std::string& value, const char* variable, const char* fallback = "") {
    if (!value.empty()) return value;
    const char* environment = std::getenv(variable);
    return environment ? environment : fallback;
}

[[maybe_unused]] std::string modelDir(const std::string& value, const ProviderContext& context, const char* folder) {
    return value.empty() ? context.modelsDir + "/" + folder : value;
}

// The provider must know its audio format before the pipeline starts.
template <class Provider>
[[maybe_unused]] std::shared_ptr<Provider> configured(std::shared_ptr<Provider> provider, const ProviderContext& context) {
    provider->setAudioConfiguration(context.audio);
    return provider;
}

void registerVad([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SILERO_VAD
    registry.add<Vad, providers::SileroOptions>("silero",
        [](const providers::SileroOptions& options, const ProviderContext& context) -> std::shared_ptr<Vad> {
            SileroVAD::SileroVADConfig sileroConfig;
            if (options.has_threshold()) sileroConfig.threshold = std::clamp(options.threshold(), 0.0f, 1.0f);
            auto vad = std::make_shared<SileroVAD>(
                options.model_path().empty() ? context.modelsDir + "/silero_vad.onnx" : options.model_path(),
                sileroConfig);
            if (!vad->initialize(context.audio, context.audio.sampleRate * 4)) return nullptr;
            return vad;
        });
#endif
}

// Cloud services first, then local models: the same preference VASDK_APP's #if chain had.
void registerStt([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_STT
    registry.add<Stt, CloudOptions>("gradium", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<GradiumppTranscriptionProvider>(
            orEnvironment(options.api_key(), "GRADIUM_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_SONIOXPP_STT
    registry.add<Stt, CloudOptions>("soniox", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<SonioxppTranscriptionProvider>(
            orEnvironment(options.api_key(), "SONIOX_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_STT
    registry.add<Stt, CloudOptions>("deepgram", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<DeepgramppTranscriptionProvider>(
            orEnvironment(options.api_key(), "DEEPGRAM_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_CARTESIAPP_STT
    registry.add<Stt, CloudOptions>("cartesia", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<CartesiappTranscriptionProvider>(
            orEnvironment(options.api_key(), "CARTESIA_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_STT
    registry.add<Stt, CloudOptions>("fishaudio", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<FishaudioppTranscriptionProvider>(
            orEnvironment(options.api_key(), "FISHAUDIO_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_WHISTLE_STT
    registry.add<Stt, LocalSttOptions>("whistle", [](const LocalSttOptions& options, const ProviderContext& context) {
        auto whistle = std::make_shared<::WhistleTranscriptionProvider>(modelDir(options.model_dir(), context, "whistle"));
        ::WhistleTranscriptionProvider::InferenceConfig inference;
        inference.idleTimeoutMs           = options.idle_timeout_ms() ? options.idle_timeout_ms() : 550;
        inference.partialIntervalMs       = options.partial_interval_ms() ? options.partial_interval_ms() : 250;
        inference.minNewSamplesForPartial = context.audio.sampleRate / 4;
        inference.minNewSamplesForFinal   = context.frameSize;
        std::string language = orEnvironment(options.language(), "WHISTLE_LANGUAGE");
        if (!language.empty()) inference.language = language;
        whistle->setInferenceConfig(inference);
        return configured(whistle, context);
    });
#endif
#ifdef VASDK_HAS_MOONSHINE_STT
    registry.add<Stt, LocalSttOptions>("moonshine", [](const LocalSttOptions& options, const ProviderContext& context) {
        auto moonshine = std::make_shared<::MoonshineSttProvider>(modelDir(options.model_dir(), context, "moonshine"));
        ::MoonshineSttProvider::InferenceConfig inference;
        inference.idleTimeoutMs     = options.idle_timeout_ms() ? options.idle_timeout_ms() : 350;
        inference.partialIntervalMs = options.partial_interval_ms() ? options.partial_interval_ms() : 160;
        moonshine->setInferenceConfig(inference);
        return configured(moonshine, context);
    });
#endif
#ifdef VASDK_HAS_PARAKEET
    registry.add<Stt, LocalSttOptions>("parakeet", [](const LocalSttOptions& options, const ProviderContext& context) {
        auto parakeet = std::make_shared<::ParakeetTranscriptionProvider>(modelDir(options.model_dir(), context, "parakeet"));
        ::ParakeetTranscriptionProvider::InferenceConfig inference;
        inference.idleTimeoutMs           = options.idle_timeout_ms() ? options.idle_timeout_ms() : 550;
        inference.partialIntervalMs       = options.partial_interval_ms() ? options.partial_interval_ms() : 250;
        inference.minNewSamplesForPartial = context.audio.sampleRate / 2;
        inference.minNewSamplesForFinal   = inference.minNewSamplesForPartial * 2;
        inference.contextSamples          = context.audio.sampleRate / 2;
        inference.silencePaddingSeconds   = 1.0f;
        parakeet->setInferenceConfig(inference);
        return configured(parakeet, context);
    });
#endif
}

void registerTts([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_TTS
    registry.add<Tts, CloudOptions>("gradium", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<GradiumppVoiceSynthesisProvider>(
            orEnvironment(options.api_key(), "GRADIUM_API_KEY"),
            orEnvironment(options.voice_id(), "GRADIUM_VOICE_ID")), context);
    });
#endif
#ifdef VASDK_HAS_SONIOXPP_TTS
    registry.add<Tts, CloudOptions>("soniox", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<SonioxppVoiceSynthesisProvider>(
            orEnvironment(options.api_key(), "SONIOX_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_SUPERTONIC_TTS
    registry.add<Tts, LocalTtsOptions>("supertonic", [](const LocalTtsOptions& options, const ProviderContext& context) {
        std::string dir = modelDir(options.model_dir(), context, "supertonic3");
        std::string voice = orEnvironment(options.voice(), "SUPERTONIC_TTS_VOICE", "M1");
        auto supertonic = std::make_shared<::SupertonicVoiceSynthesisProvider>(
            dir + "/onnx", dir + "/voice_styles/" + voice + ".json",
            orEnvironment(options.language(), "SUPERTONIC_TTS_LANG", "en"));
        // Diffusion steps trade quality for latency; synthesis time scales roughly linearly with them.
        int steps = options.steps();
        if (!steps) steps = std::getenv("SUPERTONIC_TTS_STEPS") ? std::atoi(std::getenv("SUPERTONIC_TTS_STEPS")) : 10;
        supertonic->setTotalSteps(steps);
        return configured(supertonic, context);
    });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_TTS
    registry.add<Tts, CloudOptions>("deepgram", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<DeepgramppVoiceSynthesisProvider>(
            orEnvironment(options.api_key(), "DEEPGRAM_API_KEY")), context);
    });
#endif
#ifdef VASDK_HAS_CARTESIAPP_TTS
    registry.add<Tts, CloudOptions>("cartesia", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<CartesiappVoiceSynthesisProvider>(
            orEnvironment(options.api_key(), "CARTESIA_API_KEY"),
            orEnvironment(options.voice_id(), "CARTESIA_VOICE_ID")), context);
    });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_TTS
    registry.add<Tts, CloudOptions>("fishaudio", [](const CloudOptions& options, const ProviderContext& context) {
        return configured(std::make_shared<FishaudioppVoiceSynthesisProvider>(
            orEnvironment(options.api_key(), "FISHAUDIO_API_KEY"),
            orEnvironment(options.voice_id(), "FISHAUDIO_VOICE_ID")), context);
    });
#endif
#ifdef VASDK_HAS_MOSS_TTS
    registry.add<Tts, LocalTtsOptions>("moss", [](const LocalTtsOptions& options, const ProviderContext& context) {
        auto moss = std::make_shared<::MossVoiceSynthesisProvider>(
            modelDir(options.model_dir(), context, "moss_tts"), orEnvironment(options.voice(), "MOSS_TTS_VOICE", "Ava"));
        moss->setSilenceDuration(0.3f);
        return configured(moss, context);
    });
#endif
#ifdef VASDK_HAS_PHONON_TTS
    registry.add<Tts, LocalTtsOptions>("phonon", [](const LocalTtsOptions& options, const ProviderContext& context) {
        auto phonon = std::make_shared<::PhononVoiceSynthesisProvider>(
            modelDir(options.model_dir(), context, "phonon_tts"),
            orEnvironment(options.voice(), "PHONON_TTS_VOICE", "Easton"),
            orEnvironment(options.language(), "PHONON_TTS_LANG", "en"));
        if (const char* rules = std::getenv("PHONON_TTS_RULES")) phonon->setNormalizationRules(rules);
        phonon->setSilenceDuration(0.3f);
        return configured(phonon, context);
    });
#endif
#ifdef VASDK_HAS_POCKET_TTS
    registry.add<Tts, LocalTtsOptions>("pocket", [](const LocalTtsOptions& options, const ProviderContext& context) {
        auto pocket = std::make_shared<::PocketVoiceSynthesisProvider>(
            modelDir(options.model_dir(), context, "pocket_tts"), "cosette");
        pocket->setLsdSteps(options.steps() ? options.steps() : 4);
        pocket->setVoice(orEnvironment(options.voice(), "POCKET_TTS_VOICE", "jean"));
        pocket->setPreBufferedSamplesMS(250);
        pocket->setSilenceDuration(0.3f);
        return configured(pocket, context);
    });
#endif
}

void registerTurn([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SMART_TURN_V3
    registry.add<audio::TurnDetectionProvider, providers::SmartTurnOptions>("smartturn-v3",
        [](const providers::SmartTurnOptions& options, const ProviderContext& context)
            -> std::shared_ptr<audio::TurnDetectionProvider> {
            auto smartTurn = std::make_shared<SmartTurnV3Provider>(
                options.model_path().empty() ? context.modelsDir + "/smart_turn_v3.onnx" : options.model_path(),
                0.5f, 8.0f, 3);
            smartTurn->setEvaluateOnPause(true);  // end the turn at the pause when it's clearly over
            if (!smartTurn->initialize(context.audio, context.ringBuffer)) return nullptr;
            return smartTurn;
        });
#endif
}

} // namespace

void registerAllProviders(engine::ProviderRegistry& registry) {
    registerVad(registry);
    registerStt(registry);
    registerTts(registry);
    registerTurn(registry);
}

} // namespace thirdparty
} // namespace va
