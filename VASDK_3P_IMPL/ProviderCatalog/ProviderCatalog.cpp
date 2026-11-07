#include <VASDK/c/vasdk_cpp.h>
#include "ProviderCatalog.h"

#include <algorithm>
#include <vasdk/providers/options.pb.h>

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/TurnDetectionProvider.h>
#include <VASDK/VADProvider.h>
#include <VASDK/nlu/EmbeddingNluProvider.h>

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
#ifdef VASDK_HAS_GLADIAPP
#include "GladiappTranscription/GladiaPPTranscriptionProvider.h"
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
#ifdef VASDK_HAS_MINILM_NLU
#include "MiniLmNlu/MiniLmSentenceEncoder.h"
#endif
#ifdef VASDK_HAS_GLINER_NER
#include "GlinerNer/GlinerEntityExtractor.h"
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

[[maybe_unused]] std::string caBundle(const CloudOptions& options) {
    return orEnvironment(options.ca_bundle_path(), "VASDK_CA_BUNDLE");
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

// Each provider's options struct holds its defaults; a registration fills only what the config
// (or the provider's environment variable) sets.

void registerVad([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SILERO_VAD
    registry.add<Vad, providers::SileroOptions>("silero",
        [](const providers::SileroOptions& options, const ProviderContext& context) -> std::shared_ptr<Vad> {
            SileroVADOptions silero;
            silero.modelPath = options.model_path().empty() ? context.modelsDir + "/silero_vad.onnx" : options.model_path();
            if (options.has_threshold()) silero.threshold = std::clamp(options.threshold(), 0.0f, 1.0f);
            auto vad = std::make_shared<SileroVAD>(silero);
            if (!vad->initialize(context.audio, context.audio.sampleRate * 4)) return nullptr;
            return vad;
        });
#endif
}

// Cloud services first, then local models: the same preference VASDK_APP's #if chain had.
void registerStt([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_STT
    registry.add<Stt, CloudOptions>("gradium", [](const CloudOptions& options, const ProviderContext& context) {
        GradiumppTranscriptionOptions gradium;
        gradium.apiKey = orEnvironment(options.api_key(), "GRADIUM_API_KEY");
        return configured(std::make_shared<GradiumppTranscriptionProvider>(gradium), context);
    });
#endif
#ifdef VASDK_HAS_SONIOXPP_STT
    registry.add<Stt, CloudOptions>("soniox", [](const CloudOptions& options, const ProviderContext& context) {
        SonioxppTranscriptionOptions soniox;
        soniox.apiKey       = orEnvironment(options.api_key(), "SONIOX_API_KEY");
        soniox.caBundlePath = caBundle(options);
        return configured(std::make_shared<SonioxppTranscriptionProvider>(soniox), context);
    });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_STT
    registry.add<Stt, CloudOptions>("deepgram", [](const CloudOptions& options, const ProviderContext& context) {
        DeepgramppTranscriptionOptions deepgram;
        deepgram.apiKey       = orEnvironment(options.api_key(), "DEEPGRAM_API_KEY");
        deepgram.caBundlePath = caBundle(options);
        return configured(std::make_shared<DeepgramppTranscriptionProvider>(deepgram), context);
    });
#endif
#ifdef VASDK_HAS_CARTESIAPP_STT
    registry.add<Stt, CloudOptions>("cartesia", [](const CloudOptions& options, const ProviderContext& context) {
        CartesiappTranscriptionOptions cartesia;
        cartesia.apiKey       = orEnvironment(options.api_key(), "CARTESIA_API_KEY");
        cartesia.caBundlePath = caBundle(options);
        return configured(std::make_shared<CartesiappTranscriptionProvider>(cartesia), context);
    });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_STT
    registry.add<Stt, CloudOptions>("fishaudio", [](const CloudOptions& options, const ProviderContext& context) {
        FishaudioppTranscriptionOptions fishaudio;
        fishaudio.apiKey       = orEnvironment(options.api_key(), "FISHAUDIO_API_KEY");
        fishaudio.caBundlePath = caBundle(options);
        return configured(std::make_shared<FishaudioppTranscriptionProvider>(fishaudio), context);
    });
#endif
#ifdef VASDK_HAS_GLADIAPP
    registry.add<Stt, CloudOptions>("gladia", [](const CloudOptions& options, const ProviderContext& context) {
        GladiaPPTranscriptionOptions gladia;
        gladia.apiKey       = orEnvironment(options.api_key(), "GLADIA_API_KEY");
        gladia.caBundlePath = caBundle(options);
        return configured(std::make_shared<GladiaPPTranscriptionProvider>(gladia), context);
    });
#endif
#ifdef VASDK_HAS_WHISTLE_STT
    registry.add<Stt, LocalSttOptions>("whistle", [](const LocalSttOptions& options, const ProviderContext& context) {
        WhistleTranscriptionOptions whistle;
        whistle.modelPath = modelDir(options.model_dir(), context, "whistle");
        if (options.idle_timeout_ms()) whistle.idleTimeoutMs = options.idle_timeout_ms();
        if (options.partial_interval_ms()) whistle.partialIntervalMs = options.partial_interval_ms();
        whistle.minNewSamplesForPartial = context.audio.sampleRate / 4;
        whistle.minNewSamplesForFinal   = context.frameSize;
        whistle.language = orEnvironment(options.language(), "WHISTLE_LANGUAGE", whistle.language.c_str());
        return configured(std::make_shared<WhistleTranscriptionProvider>(whistle), context);
    });
#endif
#ifdef VASDK_HAS_MOONSHINE_STT
    registry.add<Stt, LocalSttOptions>("moonshine", [](const LocalSttOptions& options, const ProviderContext& context) {
        MoonshineSttOptions moonshine;
        moonshine.modelDir = modelDir(options.model_dir(), context, "moonshine");
        if (options.idle_timeout_ms()) moonshine.idleTimeoutMs = options.idle_timeout_ms();
        if (options.partial_interval_ms()) moonshine.partialIntervalMs = options.partial_interval_ms();
        return configured(std::make_shared<MoonshineSttProvider>(moonshine), context);
    });
#endif
#ifdef VASDK_HAS_PARAKEET
    registry.add<Stt, LocalSttOptions>("parakeet", [](const LocalSttOptions& options, const ProviderContext& context) {
        ParakeetTranscriptionOptions parakeet;
        parakeet.modelDir = modelDir(options.model_dir(), context, "parakeet");
        if (options.idle_timeout_ms()) parakeet.idleTimeoutMs = options.idle_timeout_ms();
        if (options.partial_interval_ms()) parakeet.partialIntervalMs = options.partial_interval_ms();
        parakeet.minNewSamplesForPartial = context.audio.sampleRate / 2;
        parakeet.minNewSamplesForFinal   = parakeet.minNewSamplesForPartial * 2;
        parakeet.contextSamples          = context.audio.sampleRate / 2;
        return configured(std::make_shared<ParakeetTranscriptionProvider>(parakeet), context);
    });
#endif
}

void registerTts([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_TTS
    registry.add<Tts, CloudOptions>("gradium", [](const CloudOptions& options, const ProviderContext& context) {
        GradiumppVoiceSynthesisOptions gradium;
        gradium.apiKey  = orEnvironment(options.api_key(), "GRADIUM_API_KEY");
        gradium.voiceId = orEnvironment(options.voice_id(), "GRADIUM_VOICE_ID");
        return configured(std::make_shared<GradiumppVoiceSynthesisProvider>(gradium), context);
    });
#endif
#ifdef VASDK_HAS_SONIOXPP_TTS
    registry.add<Tts, CloudOptions>("soniox", [](const CloudOptions& options, const ProviderContext& context) {
        SonioxppVoiceSynthesisOptions soniox;
        soniox.apiKey       = orEnvironment(options.api_key(), "SONIOX_API_KEY");
        soniox.caBundlePath = caBundle(options);
        return configured(std::make_shared<SonioxppVoiceSynthesisProvider>(soniox), context);
    });
#endif
#ifdef VASDK_HAS_SUPERTONIC_TTS
    registry.add<Tts, LocalTtsOptions>("supertonic", [](const LocalTtsOptions& options, const ProviderContext& context) {
        const std::string dir = modelDir(options.model_dir(), context, "supertonic3");
        SupertonicVoiceSynthesisOptions supertonic;
        supertonic.onnxDir        = dir + "/onnx";
        supertonic.voiceStylePath = dir + "/voice_styles/" + orEnvironment(options.voice(), "SUPERTONIC_TTS_VOICE", "M1") + ".json";
        supertonic.language       = orEnvironment(options.language(), "SUPERTONIC_TTS_LANG", supertonic.language.c_str());
        // Diffusion steps trade quality for latency; synthesis time scales roughly linearly with them.
        if (options.steps()) supertonic.totalSteps = options.steps();
        else if (const char* steps = std::getenv("SUPERTONIC_TTS_STEPS")) supertonic.totalSteps = std::atoi(steps);
        return configured(std::make_shared<SupertonicVoiceSynthesisProvider>(supertonic), context);
    });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_TTS
    registry.add<Tts, CloudOptions>("deepgram", [](const CloudOptions& options, const ProviderContext& context) {
        DeepgramppVoiceSynthesisOptions deepgram;
        deepgram.apiKey       = orEnvironment(options.api_key(), "DEEPGRAM_API_KEY");
        deepgram.caBundlePath = caBundle(options);
        return configured(std::make_shared<DeepgramppVoiceSynthesisProvider>(deepgram), context);
    });
#endif
#ifdef VASDK_HAS_CARTESIAPP_TTS
    registry.add<Tts, CloudOptions>("cartesia", [](const CloudOptions& options, const ProviderContext& context) {
        CartesiappVoiceSynthesisOptions cartesia;
        cartesia.apiKey       = orEnvironment(options.api_key(), "CARTESIA_API_KEY");
        cartesia.voiceId      = orEnvironment(options.voice_id(), "CARTESIA_VOICE_ID");
        cartesia.caBundlePath = caBundle(options);
        return configured(std::make_shared<CartesiappVoiceSynthesisProvider>(cartesia), context);
    });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_TTS
    registry.add<Tts, CloudOptions>("fishaudio", [](const CloudOptions& options, const ProviderContext& context) {
        FishaudioppVoiceSynthesisOptions fishaudio;
        fishaudio.apiKey       = orEnvironment(options.api_key(), "FISHAUDIO_API_KEY");
        fishaudio.referenceId  = orEnvironment(options.voice_id(), "FISHAUDIO_VOICE_ID", fishaudio.referenceId.c_str());
        fishaudio.caBundlePath = caBundle(options);
        return configured(std::make_shared<FishaudioppVoiceSynthesisProvider>(fishaudio), context);
    });
#endif
#ifdef VASDK_HAS_MOSS_TTS
    registry.add<Tts, LocalTtsOptions>("moss", [](const LocalTtsOptions& options, const ProviderContext& context) {
        MossVoiceSynthesisOptions moss;
        moss.modelDir = modelDir(options.model_dir(), context, "moss_tts");
        moss.voice    = orEnvironment(options.voice(), "MOSS_TTS_VOICE", moss.voice.c_str());
        if (options.prebuffer_ms() > 0) moss.prebufferMs = options.prebuffer_ms();
        return configured(std::make_shared<MossVoiceSynthesisProvider>(moss), context);
    });
#endif
#ifdef VASDK_HAS_PHONON_TTS
    registry.add<Tts, LocalTtsOptions>("phonon", [](const LocalTtsOptions& options, const ProviderContext& context) {
        PhononVoiceSynthesisOptions phonon;
        phonon.modelDir = modelDir(options.model_dir(), context, "phonon_tts");
        phonon.voice    = orEnvironment(options.voice(), "PHONON_TTS_VOICE", phonon.voice.c_str());
        phonon.language = orEnvironment(options.language(), "PHONON_TTS_LANG", phonon.language.c_str());
        phonon.normalizationRules = orEnvironment("", "PHONON_TTS_RULES", phonon.normalizationRules.c_str());
        return configured(std::make_shared<PhononVoiceSynthesisProvider>(phonon), context);
    });
#endif
#ifdef VASDK_HAS_POCKET_TTS
    registry.add<Tts, LocalTtsOptions>("pocket", [](const LocalTtsOptions& options, const ProviderContext& context) {
        PocketVoiceSynthesisOptions pocket;
        pocket.modelDir = modelDir(options.model_dir(), context, "pocket_tts");
        pocket.voice    = orEnvironment(options.voice(), "POCKET_TTS_VOICE", pocket.voice.c_str());
        if (options.steps()) pocket.lsdSteps = options.steps();
        if (options.prebuffer_ms() > 0) pocket.prebufferMs = options.prebuffer_ms();
        return configured(std::make_shared<PocketVoiceSynthesisProvider>(pocket), context);
    });
#endif
}

void registerTurn([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SMART_TURN_V3
    registry.add<audio::TurnDetectionProvider, providers::SmartTurnOptions>("smartturn-v3",
        [](const providers::SmartTurnOptions& options, const ProviderContext& context)
            -> std::shared_ptr<audio::TurnDetectionProvider> {
            SmartTurnV3Options smartTurnOptions;
            smartTurnOptions.modelPath =
                options.model_path().empty() ? context.modelsDir + "/smart_turn_v3.onnx" : options.model_path();
            auto smartTurn = std::make_shared<SmartTurnV3Provider>(smartTurnOptions);
            if (!smartTurn->initialize(context.audio, context.ringBuffer)) return nullptr;
            return smartTurn;
        });
#endif
}

void registerEncoders([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_MINILM_NLU
    registry.add<nlu::SentenceEncoder, providers::MiniLmOptions>("minilm",
        [](const providers::MiniLmOptions& options, const ProviderContext& context) -> std::shared_ptr<nlu::SentenceEncoder> {
            MiniLmSentenceEncoderOptions minilm;
            minilm.modelDir = modelDir(options.model_dir(), context, "minilm");
            auto encoder = std::make_shared<MiniLmSentenceEncoder>(minilm);
            if (!encoder->initialize()) return nullptr;
            return encoder;
        });
#endif
#ifdef VASDK_HAS_GLINER_NER
    registry.add<nlu::EntityExtractor, providers::GlinerOptions>("gliner",
        [](const providers::GlinerOptions& options, const ProviderContext& context) -> std::shared_ptr<nlu::EntityExtractor> {
            GlinerEntityExtractorOptions gliner;
            gliner.modelDir = modelDir(options.model_dir(), context, "gliner");
            if (options.has_threshold()) gliner.threshold = options.threshold();
            auto extractor = std::make_shared<GlinerEntityExtractor>(gliner);
            if (!extractor->initialize()) return nullptr;
            return extractor;
        });
#endif
}

} // namespace

void registerAllProviders(engine::ProviderRegistry& registry) {
    registerEncoders(registry);
    registerVad(registry);
    registerStt(registry);
    registerTts(registry);
    registerTurn(registry);
}

} // namespace thirdparty
} // namespace va

