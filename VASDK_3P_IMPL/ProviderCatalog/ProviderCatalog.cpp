#include <VASDK/c/vasdk_cpp.h>
#include "ProviderCatalog.h"

#include <algorithm>
#include <vasdk/providers/options.pb.h>

#include <VASDK/GenericAudioDSPProvider.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/SpeakerEmbeddingProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/TurnDetectionProvider.h>
#include <VASDK/VADProvider.h>
#include <VASDK/nlu/EmbeddingNluProvider.h>

#include <cctype>
#include <cstdlib>
#include <optional>
#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_MINIAUDIO
#include "MiniAudio/MiniAudioSink.h"
#include "MiniAudio/MiniAudioSource.h"
#endif
#ifdef VASDK_HAS_AAUDIO
#include "AAudio/AAudioSink.h"
#include "AAudio/AAudioSource.h"
#endif
#ifdef VASDK_HAS_LLAMACPP
#include "LlamaCpp/LlamaCppCompletion.h"
#include "LlamaCpp/LlamaCppTransport.h"
#include <VASDK/llm/ResponseChunker.h>
#endif
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
#ifdef VASDK_HAS_TURN1MINI
#include "Turn1Mini/Turn1MiniProvider.h"
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
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
#include "LibDataChannelWebRtc/LibDataChannelWebRtcProvider.h"
#endif
#ifdef VASDK_HAS_RNNOISE
#include "RNNoise/RNNoiseDSPProvider.h"
#endif
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
#include "PyannoteSpeaker/PyannoteEmbeddingProvider.h"
#endif

namespace va {
namespace thirdparty {

namespace {

namespace providers = ::vasdk::providers;
using engine::ProviderContext;
using Stt = va::audio::GenericTranscriptionProvider;
using Tts = va::audio::GenericVoiceSynthesisProvider;
using Vad = va::audio::VADProvider;
using providers::CloudOptions;
using providers::LocalSttOptions;
using providers::LocalTtsOptions;

// Config value, else the environment variable, else the fallback.
[[maybe_unused]] std::string orEnvironment(const std::string& value, const char* variable, const char* fallback = "") {
    if (!value.empty()) return value;
    const char* environment = std::getenv(variable);
    return environment ? environment : fallback;
}

template <class Message>
[[maybe_unused]] std::string caBundle(const Message& options) {
    return orEnvironment(options.ca_bundle_path(), "VASDK_CA_BUNDLE");
}

[[maybe_unused]] std::string modelDir(const std::string& value, const ProviderContext& context, const char* folder) {
    return value.empty() ? context.modelsDir + "/" + folder : value;
}

// The voice's own prebuffer_ms, else the engine's audio.tts_prebuffer_ms, else the options struct's default.
template <class Message>
[[maybe_unused]] int prebufferMs(const Message& options, const ProviderContext& context, int providerDefault) {
    if (options.has_prebuffer_ms()) return std::max(0, options.prebuffer_ms());
    return context.ttsPrebufferMs >= 0 ? context.ttsPrebufferMs : providerDefault;
}

// The provider must know its audio format before the pipeline starts.
template <class Provider>
[[maybe_unused]] std::shared_ptr<Provider> configured(std::shared_ptr<Provider> provider, const ProviderContext& context) {
    provider->setAudioConfiguration(context.audio);
    return provider;
}

// A cloud provider can't start without its key: no provider then, so the spec's next fallback
// starts instead (ProviderRegistry::create), as it does for a local model that won't load.
template <class Provider, class Options>
[[maybe_unused]] std::shared_ptr<Provider> cloud(const Options& options, const char* keyVariable, const ProviderContext& context) {
    if (options.apiKey.empty()) {
        spdlog::warn("[ProviderCatalog] no API key: set the provider's api_key option or {}", keyVariable);
        return nullptr;
    }
    return configured(std::make_shared<Provider>(options), context);
}

// ---- The shared messages of older configs, in each provider's own message ----

template <class Message>
[[maybe_unused]] Message keyFrom(const CloudOptions& shared) {
    Message own;
    own.set_api_key(shared.api_key());
    return own;
}

template <class Message>
[[maybe_unused]] Message keyAndCertificatesFrom(const CloudOptions& shared) {
    Message own = keyFrom<Message>(shared);
    own.set_ca_bundle_path(shared.ca_bundle_path());
    return own;
}

template <class Message>
[[maybe_unused]] Message localSttFrom(const LocalSttOptions& shared) {
    Message own;
    own.set_model_dir(shared.model_dir());
    if (shared.idle_timeout_ms()) own.set_idle_timeout_ms(shared.idle_timeout_ms());
    if (shared.partial_interval_ms()) own.set_partial_interval_ms(shared.partial_interval_ms());
    return own;
}

// model_dir, voice and prebuffer_ms (0 was "unset" there).
template <class Message>
[[maybe_unused]] Message localTtsFrom(const LocalTtsOptions& shared) {
    Message own;
    own.set_model_dir(shared.model_dir());
    own.set_voice(shared.voice());
    if (shared.prebuffer_ms() > 0) own.set_prebuffer_ms(shared.prebuffer_ms());
    return own;
}

// Each provider's options struct holds its defaults; a registration fills only what the config
// (or the provider's environment variable) sets.

void registerVad([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SILERO_VAD
    registry.add<Vad, providers::SileroOptions>("silero",
        [](const providers::SileroOptions& options, const ProviderContext& context) -> std::shared_ptr<Vad> {
            vad::SileroVADOptions silero;
            silero.modelPath = options.model_path().empty() ? context.modelsDir + "/silero_vad.onnx" : options.model_path();
            if (options.has_threshold()) silero.threshold = std::clamp(options.threshold(), 0.0f, 1.0f);
            if (options.has_window_frame_size_ms()) silero.windowFrameSizeMs = options.window_frame_size_ms();
            if (options.has_min_silence_duration_ms()) silero.minSilenceDurationMs = options.min_silence_duration_ms();
            if (options.has_speech_pad_ms()) silero.speechPadMs = options.speech_pad_ms();
            if (options.has_min_speech_duration_ms()) silero.minSpeechDurationMs = options.min_speech_duration_ms();
            auto detector = std::make_shared<vad::SileroVAD>(silero);
            if (!detector->initialize(context.audio, context.audio.sampleRate * 4)) return nullptr;
            return detector;
        });
#endif
}

// Cloud services first, then local models: the same preference VASDK_APP's #if chain had.
void registerStt([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_STT
    registry.add<Stt, providers::GradiumSttOptions, CloudOptions>("gradium",
        [](const providers::GradiumSttOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::GradiumppTranscriptionOptions gradium;
            gradium.apiKey = orEnvironment(options.api_key(), "GRADIUM_API_KEY");
            if (!options.model().empty()) gradium.model = options.model();
            if (!options.language().empty()) gradium.language = options.language();
            return cloud<stt::GradiumppTranscriptionProvider>(gradium, "GRADIUM_API_KEY", context);
        },
        keyFrom<providers::GradiumSttOptions>);
#endif
#ifdef VASDK_HAS_SONIOXPP_STT
    registry.add<Stt, providers::SonioxSttOptions, CloudOptions>("soniox",
        [](const providers::SonioxSttOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::SonioxppTranscriptionOptions soniox;
            soniox.apiKey       = orEnvironment(options.api_key(), "SONIOX_API_KEY");
            soniox.caBundlePath = caBundle(options);
            if (!options.model().empty()) soniox.model = options.model();
            if (options.has_endpoint_detection()) soniox.endpointDetection = options.endpoint_detection();
            return cloud<stt::SonioxppTranscriptionProvider>(soniox, "SONIOX_API_KEY", context);
        },
        keyAndCertificatesFrom<providers::SonioxSttOptions>);
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_STT
    registry.add<Stt, providers::DeepgramSttOptions, CloudOptions>("deepgram",
        [](const providers::DeepgramSttOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::DeepgramppTranscriptionOptions deepgram;
            deepgram.apiKey       = orEnvironment(options.api_key(), "DEEPGRAM_API_KEY");
            deepgram.caBundlePath = caBundle(options);
            if (!options.model().empty()) deepgram.model = options.model();
            if (!options.language().empty()) deepgram.language = options.language();
            if (options.has_interim_results()) deepgram.interimResults = options.interim_results();
            if (options.has_smart_format()) deepgram.smartFormat = options.smart_format();
            if (options.has_endpointing_ms()) deepgram.endpointingMs = options.endpointing_ms();
            if (options.has_utterance_end_ms()) deepgram.utteranceEndMs = options.utterance_end_ms();
            return cloud<stt::DeepgramppTranscriptionProvider>(deepgram, "DEEPGRAM_API_KEY", context);
        },
        keyAndCertificatesFrom<providers::DeepgramSttOptions>);
#endif
#ifdef VASDK_HAS_CARTESIAPP_STT
    registry.add<Stt, providers::CartesiaSttOptions, CloudOptions>("cartesia",
        [](const providers::CartesiaSttOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::CartesiappTranscriptionOptions cartesia;
            cartesia.apiKey       = orEnvironment(options.api_key(), "CARTESIA_API_KEY");
            cartesia.caBundlePath = caBundle(options);
            if (!options.model().empty()) cartesia.model = options.model();
            if (!options.language().empty()) cartesia.language = options.language();
            return cloud<stt::CartesiappTranscriptionProvider>(cartesia, "CARTESIA_API_KEY", context);
        },
        keyAndCertificatesFrom<providers::CartesiaSttOptions>);
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_STT
    registry.add<Stt, providers::FishAudioSttOptions, CloudOptions>("fishaudio",
        [](const providers::FishAudioSttOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::FishaudioppTranscriptionOptions fishaudio;
            fishaudio.apiKey       = orEnvironment(options.api_key(), "FISHAUDIO_API_KEY");
            fishaudio.caBundlePath = caBundle(options);
            if (!options.language().empty()) fishaudio.language = options.language();
            return cloud<stt::FishaudioppTranscriptionProvider>(fishaudio, "FISHAUDIO_API_KEY", context);
        },
        keyAndCertificatesFrom<providers::FishAudioSttOptions>);
#endif
#ifdef VASDK_HAS_GLADIAPP
    registry.add<Stt, providers::GladiaOptions, CloudOptions>("gladia",
        [](const providers::GladiaOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::GladiaPPTranscriptionOptions gladia;
            gladia.apiKey       = orEnvironment(options.api_key(), "GLADIA_API_KEY");
            gladia.caBundlePath = caBundle(options);
            if (options.has_region()) gladia.region = options.region() == providers::GladiaOptions::EU_WEST ? "eu-west" : "us-west";
            if (options.has_endpointing_seconds()) gladia.endpointingSeconds = options.endpointing_seconds();
            if (options.has_max_duration_without_endpointing_seconds())
                gladia.maxDurationWithoutEndpointingSeconds = options.max_duration_without_endpointing_seconds();
            return cloud<stt::GladiaPPTranscriptionProvider>(gladia, "GLADIA_API_KEY", context);
        },
        keyAndCertificatesFrom<providers::GladiaOptions>);
#endif
#ifdef VASDK_HAS_WHISTLE_STT
    registry.add<Stt, providers::WhistleOptions, LocalSttOptions>("whistle",
        [](const providers::WhistleOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::WhistleTranscriptionOptions whistle;
            whistle.modelPath = modelDir(options.model_dir(), context, "whistle");
            if (options.has_idle_timeout_ms()) whistle.idleTimeoutMs = options.idle_timeout_ms();
            if (options.has_partial_interval_ms()) whistle.partialIntervalMs = options.partial_interval_ms();
            whistle.minNewSamplesForPartial = options.has_min_new_samples_for_partial()
                ? static_cast<size_t>(std::max(0, options.min_new_samples_for_partial())) : static_cast<size_t>(context.audio.sampleRate / 4);
            whistle.minNewSamplesForFinal = options.has_min_new_samples_for_final()
                ? static_cast<size_t>(std::max(0, options.min_new_samples_for_final())) : static_cast<size_t>(context.frameSize);
            whistle.language = orEnvironment(options.language(), "WHISTLE_LANGUAGE", whistle.language.c_str());
            if (!options.engine_path().empty()) whistle.enginePath = options.engine_path();
            whistle.keywords.assign(options.keywords().begin(), options.keywords().end());
            if (options.has_word_timestamps()) whistle.wordTimestamps = options.word_timestamps();
            return configured(std::make_shared<stt::WhistleTranscriptionProvider>(whistle), context);
        },
        [](const LocalSttOptions& shared) {
            auto own = localSttFrom<providers::WhistleOptions>(shared);
            own.set_language(shared.language());
            return own;
        });
#endif
#ifdef VASDK_HAS_MOONSHINE_STT
    registry.add<Stt, providers::MoonshineOptions, LocalSttOptions>("moonshine",
        [](const providers::MoonshineOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            stt::MoonshineSttOptions moonshine;
            moonshine.modelDir = modelDir(options.model_dir(), context, "moonshine");
            if (options.has_idle_timeout_ms()) moonshine.idleTimeoutMs = options.idle_timeout_ms();
            if (options.has_partial_interval_ms()) moonshine.partialIntervalMs = options.partial_interval_ms();
            return configured(std::make_shared<stt::MoonshineSttProvider>(moonshine), context);
        },
        localSttFrom<providers::MoonshineOptions>);
#endif
#ifdef VASDK_HAS_PARAKEET
    registry.add<Stt, providers::ParakeetOptions, LocalSttOptions>("parakeet",
        [](const providers::ParakeetOptions& options, const ProviderContext& context) -> std::shared_ptr<Stt> {
            auto samples = [](int value) { return static_cast<size_t>(std::max(0, value)); };
            stt::ParakeetTranscriptionOptions parakeet;
            parakeet.modelDir = modelDir(options.model_dir(), context, "parakeet");
            if (options.has_idle_timeout_ms()) parakeet.idleTimeoutMs = options.idle_timeout_ms();
            if (options.has_partial_interval_ms()) parakeet.partialIntervalMs = options.partial_interval_ms();
            if (options.has_incremental_partials()) parakeet.enableIncrementalPartials = options.incremental_partials();
            parakeet.minNewSamplesForPartial = options.has_min_new_samples_for_partial()
                ? samples(options.min_new_samples_for_partial()) : samples(context.audio.sampleRate / 2);
            parakeet.minNewSamplesForFinal = options.has_min_new_samples_for_final()
                ? samples(options.min_new_samples_for_final()) : parakeet.minNewSamplesForPartial * 2;
            parakeet.contextSamples = options.has_context_samples()
                ? samples(options.context_samples()) : samples(context.audio.sampleRate / 2);
            if (options.has_high_confidence_shortcut_samples())
                parakeet.highConfShortcutSamples = samples(options.high_confidence_shortcut_samples());
            if (options.has_high_confidence_shortcut_threshold())
                parakeet.highConfShortcutThreshold = std::clamp(options.high_confidence_shortcut_threshold(), 0.0f, 1.0f);
            if (options.has_context_boundary_tolerance_ms()) parakeet.contextBoundaryToleranceMs = options.context_boundary_tolerance_ms();
            if (options.has_max_buffer_seconds()) parakeet.maxBufferSeconds = options.max_buffer_seconds();
            if (options.has_silence_padding_seconds()) parakeet.silencePaddingSeconds = options.silence_padding_seconds();
            if (options.has_overlap_boundary_tolerance_ms()) parakeet.overlapBoundaryToleranceMs = options.overlap_boundary_tolerance_ms();
            if (options.has_end_padding_tolerance_ms()) parakeet.endPaddingToleranceMs = options.end_padding_tolerance_ms();
            return configured(std::make_shared<stt::ParakeetTranscriptionProvider>(parakeet), context);
        },
        localSttFrom<providers::ParakeetOptions>);
#endif
}

void registerTts([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_TTS
    registry.add<Tts, providers::GradiumTtsOptions, CloudOptions>("gradium",
        [](const providers::GradiumTtsOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::GradiumppVoiceSynthesisOptions gradium;
            gradium.apiKey  = orEnvironment(options.api_key(), "GRADIUM_API_KEY");
            gradium.voiceId = orEnvironment(options.voice_id(), "GRADIUM_VOICE_ID");
            if (!options.model().empty()) gradium.model = options.model();
            return cloud<tts::GradiumppVoiceSynthesisProvider>(gradium, "GRADIUM_API_KEY", context);
        },
        [](const CloudOptions& shared) {
            auto own = keyFrom<providers::GradiumTtsOptions>(shared);
            own.set_voice_id(shared.voice_id());
            return own;
        });
#endif
#ifdef VASDK_HAS_SONIOXPP_TTS
    registry.add<Tts, providers::SonioxTtsOptions, CloudOptions>("soniox",
        [](const providers::SonioxTtsOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::SonioxppVoiceSynthesisOptions soniox;
            soniox.apiKey       = orEnvironment(options.api_key(), "SONIOX_API_KEY");
            soniox.voice        = orEnvironment(options.voice(), "SONIOX_VOICE_ID", soniox.voice.c_str());
            soniox.caBundlePath = caBundle(options);
            if (!options.language().empty()) soniox.language = options.language();
            return cloud<tts::SonioxppVoiceSynthesisProvider>(soniox, "SONIOX_API_KEY", context);
        },
        [](const CloudOptions& shared) {
            auto own = keyAndCertificatesFrom<providers::SonioxTtsOptions>(shared);
            own.set_voice(shared.voice_id());
            return own;
        });
#endif
#ifdef VASDK_HAS_SUPERTONIC_TTS
    registry.add<Tts, providers::SupertonicOptions, LocalTtsOptions>("supertonic",
        [](const providers::SupertonicOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            const std::string dir = modelDir(options.model_dir(), context, "supertonic3");
            tts::SupertonicVoiceSynthesisOptions supertonic;
            supertonic.onnxDir        = dir + "/onnx";
            supertonic.voiceStylePath = dir + "/voice_styles/" + orEnvironment(options.voice(), "SUPERTONIC_TTS_VOICE", "M1") + ".json";
            supertonic.language       = orEnvironment(options.language(), "SUPERTONIC_TTS_LANG", supertonic.language.c_str());
            // Diffusion steps trade quality for latency; synthesis time scales roughly linearly with them.
            if (options.has_steps()) supertonic.totalSteps = options.steps();
            else if (const char* steps = std::getenv("SUPERTONIC_TTS_STEPS")) supertonic.totalSteps = std::atoi(steps);
            supertonic.prebufferMs = prebufferMs(options, context, supertonic.prebufferMs);
            if (options.has_speed()) supertonic.speed = options.speed();
            if (options.has_silence_duration_seconds()) supertonic.silenceDurationSeconds = options.silence_duration_seconds();
            if (options.has_trim_margin()) supertonic.trimMargin = options.trim_margin();
            return configured(std::make_shared<tts::SupertonicVoiceSynthesisProvider>(supertonic), context);
        },
        [](const LocalTtsOptions& shared) {
            auto own = localTtsFrom<providers::SupertonicOptions>(shared);
            own.set_language(shared.language());
            if (shared.steps()) own.set_steps(shared.steps());
            return own;
        });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_TTS
    registry.add<Tts, providers::DeepgramTtsOptions, CloudOptions>("deepgram",
        [](const providers::DeepgramTtsOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::DeepgramppVoiceSynthesisOptions deepgram;
            deepgram.apiKey       = orEnvironment(options.api_key(), "DEEPGRAM_API_KEY");
            deepgram.model        = orEnvironment(options.model(), "DEEPGRAM_VOICE_ID", deepgram.model.c_str());
            deepgram.caBundlePath = caBundle(options);
            return cloud<tts::DeepgramppVoiceSynthesisProvider>(deepgram, "DEEPGRAM_API_KEY", context);
        },
        [](const CloudOptions& shared) {
            auto own = keyAndCertificatesFrom<providers::DeepgramTtsOptions>(shared);
            own.set_model(shared.voice_id());   // in Aura the model is the voice
            return own;
        });
#endif
#ifdef VASDK_HAS_CARTESIAPP_TTS
    registry.add<Tts, providers::CartesiaTtsOptions, CloudOptions>("cartesia",
        [](const providers::CartesiaTtsOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::CartesiappVoiceSynthesisOptions cartesia;
            cartesia.apiKey       = orEnvironment(options.api_key(), "CARTESIA_API_KEY");
            cartesia.voiceId      = orEnvironment(options.voice_id(), "CARTESIA_VOICE_ID");
            cartesia.caBundlePath = caBundle(options);
            if (!options.model().empty()) cartesia.model = options.model();
            if (!options.language().empty()) cartesia.language = options.language();
            return cloud<tts::CartesiappVoiceSynthesisProvider>(cartesia, "CARTESIA_API_KEY", context);
        },
        [](const CloudOptions& shared) {
            auto own = keyAndCertificatesFrom<providers::CartesiaTtsOptions>(shared);
            own.set_voice_id(shared.voice_id());
            return own;
        });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_TTS
    registry.add<Tts, providers::FishAudioTtsOptions, CloudOptions>("fishaudio",
        [](const providers::FishAudioTtsOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::FishaudioppVoiceSynthesisOptions fishaudio;
            fishaudio.apiKey       = orEnvironment(options.api_key(), "FISHAUDIO_API_KEY");
            fishaudio.referenceId  = orEnvironment(options.reference_id(), "FISHAUDIO_VOICE_ID", fishaudio.referenceId.c_str());
            fishaudio.caBundlePath = caBundle(options);
            if (!options.model().empty()) fishaudio.model = options.model();
            return cloud<tts::FishaudioppVoiceSynthesisProvider>(fishaudio, "FISHAUDIO_API_KEY", context);
        },
        [](const CloudOptions& shared) {
            auto own = keyAndCertificatesFrom<providers::FishAudioTtsOptions>(shared);
            own.set_reference_id(shared.voice_id());
            return own;
        });
#endif
#ifdef VASDK_HAS_MOSS_TTS
    registry.add<Tts, providers::MossOptions, LocalTtsOptions>("moss",
        [](const providers::MossOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::MossVoiceSynthesisOptions moss;
            moss.modelDir    = modelDir(options.model_dir(), context, "moss_tts");
            moss.voice       = orEnvironment(options.voice(), "MOSS_TTS_VOICE", moss.voice.c_str());
            moss.prebufferMs = prebufferMs(options, context, moss.prebufferMs);
            if (options.has_threads()) moss.threads = options.threads();
            if (options.has_silence_duration_seconds()) moss.silenceDurationSeconds = options.silence_duration_seconds();
            if (options.has_max_new_frames()) moss.maxNewFrames = options.max_new_frames();
            if (options.has_max_chunk_tokens()) moss.maxChunkTokens = options.max_chunk_tokens();
            if (options.has_text_normalization()) moss.textNormalization = options.text_normalization();
            if (options.has_pause_short_seconds()) moss.pauseShortSeconds = options.pause_short_seconds();
            if (options.has_pause_long_seconds()) moss.pauseLongSeconds = options.pause_long_seconds();
            return configured(std::make_shared<tts::MossVoiceSynthesisProvider>(moss), context);
        },
        localTtsFrom<providers::MossOptions>);
#endif
#ifdef VASDK_HAS_PHONON_TTS
    registry.add<Tts, providers::PhononOptions, LocalTtsOptions>("phonon",
        [](const providers::PhononOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::PhononVoiceSynthesisOptions phonon;
            phonon.modelDir = modelDir(options.model_dir(), context, "phonon_tts");
            phonon.voice    = orEnvironment(options.voice(), "PHONON_TTS_VOICE", phonon.voice.c_str());
            phonon.language = orEnvironment(options.language(), "PHONON_TTS_LANG", phonon.language.c_str());
            phonon.normalizationRules = orEnvironment(options.normalization_rules(), "PHONON_TTS_RULES", phonon.normalizationRules.c_str());
            phonon.prebufferMs = prebufferMs(options, context, phonon.prebufferMs);
            if (options.has_temperature()) phonon.temperature = options.temperature();
            if (options.has_duration_delta()) phonon.durationDelta = options.duration_delta();
            if (options.has_stream_chunk_frames()) phonon.streamChunkFrames = options.stream_chunk_frames();
            if (options.has_max_tokens_per_chunk()) phonon.maxTokensPerChunk = options.max_tokens_per_chunk();
            if (options.has_silence_duration_seconds()) phonon.silenceDurationSeconds = options.silence_duration_seconds();
            return configured(std::make_shared<tts::PhononVoiceSynthesisProvider>(phonon), context);
        },
        [](const LocalTtsOptions& shared) {
            auto own = localTtsFrom<providers::PhononOptions>(shared);
            own.set_language(shared.language());
            return own;
        });
#endif
#ifdef VASDK_HAS_POCKET_TTS
    registry.add<Tts, providers::PocketOptions, LocalTtsOptions>("pocket",
        [](const providers::PocketOptions& options, const ProviderContext& context) -> std::shared_ptr<Tts> {
            tts::PocketVoiceSynthesisOptions pocket;
            pocket.modelDir    = modelDir(options.model_dir(), context, "pocket_tts");
            pocket.voice       = orEnvironment(options.voice(), "POCKET_TTS_VOICE", pocket.voice.c_str());
            pocket.prebufferMs = prebufferMs(options, context, pocket.prebufferMs);
            if (options.has_steps()) pocket.lsdSteps = options.steps();
            if (options.has_temperature()) pocket.temperature = options.temperature();
            if (options.has_silence_duration_seconds()) pocket.silenceDurationSeconds = options.silence_duration_seconds();
            if (options.has_decoder_batch_frames()) pocket.decoderBatchFrames = options.decoder_batch_frames();
            return configured(std::make_shared<tts::PocketVoiceSynthesisProvider>(pocket), context);
        },
        [](const LocalTtsOptions& shared) {
            auto own = localTtsFrom<providers::PocketOptions>(shared);
            if (shared.steps()) own.set_steps(shared.steps());
            return own;
        });
#endif
}

void registerTurn([[maybe_unused]] engine::ProviderRegistry& registry) {
    // Registration order is the preference: turn-1-mini first (docs/providers/turn-1-mini.md, "Evaluation");
    // it is English only, so other languages name smartturn-v3
#ifdef VASDK_HAS_TURN1MINI
    registry.add<va::audio::TurnDetectionProvider, providers::Turn1MiniOptions>("turn-1-mini",
        [](const providers::Turn1MiniOptions& options, const ProviderContext& context)
            -> std::shared_ptr<va::audio::TurnDetectionProvider> {
            turn::Turn1MiniOptions turn1Mini;
            turn1Mini.modelPath = options.model_path().empty() ? context.modelsDir + "/turn_1_mini.onnx" : options.model_path();
            if (options.has_threshold()) turn1Mini.threshold = options.threshold();
            else if (const char* threshold = std::getenv("VASDK_TURN1MINI_THRESHOLD")) turn1Mini.threshold = std::strtof(threshold, nullptr);
            if (options.has_pause_threshold()) turn1Mini.pauseThreshold = options.pause_threshold();
            else if (const char* pause = std::getenv("VASDK_TURN1MINI_PAUSE_THRESHOLD")) turn1Mini.pauseThreshold = std::strtof(pause, nullptr);
            turn1Mini.threshold = std::clamp(turn1Mini.threshold, 0.0f, 1.0f);
            turn1Mini.pauseThreshold = std::clamp(turn1Mini.pauseThreshold, 0.0f, 1.0f);
            if (options.has_post_audio_frames()) turn1Mini.postAudioFrames = std::max(0, options.post_audio_frames());
            if (options.has_evaluate_on_pause()) turn1Mini.evaluateOnPause = options.evaluate_on_pause();
            auto provider = std::make_shared<turn::Turn1MiniProvider>(turn1Mini);
            if (!provider->initialize(context.audio, context.ringBuffer)) return nullptr;
            return provider;
        });
#endif
#ifdef VASDK_HAS_SMART_TURN_V3
    registry.add<va::audio::TurnDetectionProvider, providers::SmartTurnOptions>("smartturn-v3",
        [](const providers::SmartTurnOptions& options, const ProviderContext& context)
            -> std::shared_ptr<va::audio::TurnDetectionProvider> {
            turn::SmartTurnV3Options smartTurnOptions;
            smartTurnOptions.modelPath =
                options.model_path().empty() ? context.modelsDir + "/smart_turn_v3.onnx" : options.model_path();
            if (options.has_threshold()) smartTurnOptions.threshold = std::clamp(options.threshold(), 0.0f, 1.0f);
            if (options.has_max_audio_duration_seconds()) smartTurnOptions.maxAudioDurationSeconds = options.max_audio_duration_seconds();
            if (options.has_post_audio_frames()) smartTurnOptions.postAudioFrames = std::max(0, options.post_audio_frames());
            if (options.has_evaluate_on_pause()) smartTurnOptions.evaluateOnPause = options.evaluate_on_pause();
            if (options.has_pause_threshold()) smartTurnOptions.pauseThreshold = std::clamp(options.pause_threshold(), 0.0f, 1.0f);
            auto smartTurn = std::make_shared<turn::SmartTurnV3Provider>(smartTurnOptions);
            if (!smartTurn->initialize(context.audio, context.ringBuffer)) return nullptr;
            return smartTurn;
        });
#endif
}

void registerEncoders([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_MINILM_NLU
    registry.add<va::nlu::SentenceEncoder, providers::MiniLmOptions>("minilm",
        [](const providers::MiniLmOptions& options, const ProviderContext& context) -> std::shared_ptr<va::nlu::SentenceEncoder> {
            nlu::MiniLmSentenceEncoderOptions minilm;
            minilm.modelDir = modelDir(options.model_dir(), context, "minilm");
            if (options.has_threads()) minilm.threads = options.threads();
            auto encoder = std::make_shared<nlu::MiniLmSentenceEncoder>(minilm);
            if (!encoder->initialize()) return nullptr;
            return encoder;
        });
#endif
#ifdef VASDK_HAS_GLINER_NER
    registry.add<va::nlu::EntityExtractor, providers::GlinerOptions>("gliner",
        [](const providers::GlinerOptions& options, const ProviderContext& context) -> std::shared_ptr<va::nlu::EntityExtractor> {
            nlu::GlinerEntityExtractorOptions gliner;
            gliner.modelDir = modelDir(options.model_dir(), context, "gliner");
            if (options.has_threshold()) gliner.threshold = options.threshold();
            if (options.has_threads()) gliner.threads = options.threads();
            auto extractor = std::make_shared<nlu::GlinerEntityExtractor>(gliner);
            if (!extractor->initialize()) return nullptr;
            return extractor;
        });
#endif
}

void registerAudioDsp([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_RNNOISE
    // The runtime initializes it with the session's input format.
    registry.add<va::audio::GenericAudioDSPProvider, providers::RNNoiseOptions>("rnnoise",
        [](const providers::RNNoiseOptions& options, const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioDSPProvider> {
            audio::RNNoiseDSPOptions rnnoise;
            rnnoise.modelPath = options.model_path();
            if (options.has_vad_threshold()) rnnoise.vadThreshold = std::clamp(options.vad_threshold(), 0.0f, 1.0f);
            rnnoise.normalizeOutput = options.normalize_output();
            if (options.has_normalization_multiplier()) rnnoise.normalizationMultiplier = options.normalization_multiplier();
            return std::make_shared<audio::RNNoiseDSPProvider>(rnnoise);
        });
#endif
}

void registerSpeakerEmbedding([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    registry.add<va::audio::SpeakerEmbeddingProvider, providers::PyannoteOptions>("pyannote",
        [](const providers::PyannoteOptions& options, const ProviderContext& context)
            -> std::shared_ptr<va::audio::SpeakerEmbeddingProvider> {
            speaker::PyannoteEmbeddingOptions pyannote;
            pyannote.modelPath = options.model_path().empty() ? context.modelsDir + "/pyannote_embedding.onnx" : options.model_path();
            auto embedding = std::make_shared<speaker::PyannoteEmbeddingProvider>(pyannote);
            if (!embedding->initialize(context.audio)) return nullptr;
            return embedding;
        });
#endif
}

void registerWebRtc([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_LIBDATACHANNEL_WEBRTC
    registry.add<va::network::WebRtcInterface, providers::LibDataChannelWebRtcOptions>("libdatachannel",
        [](const providers::LibDataChannelWebRtcOptions& options, const ProviderContext&) -> std::shared_ptr<va::network::WebRtcInterface> {
            transport::LibDataChannelWebRtcOptions webrtc;
            if (options.ice_servers_size() > 0) webrtc.iceServers.assign(options.ice_servers().begin(), options.ice_servers().end());
            const std::string port = orEnvironment(options.port() ? std::to_string(options.port()) : "", "VASDK_WEBRTC_PORT");
            if (!port.empty()) webrtc.signalingPort = static_cast<uint16_t>(std::atoi(port.c_str()));
            if (!options.host().empty()) webrtc.signalingHost = options.host();
            const std::string page = orEnvironment(options.page(), "VASDK_WEBRTC_PAGE", VASDK_WEBRTC_DEMO_PAGE);
            auto peer = std::make_shared<transport::LibDataChannelWebRtcProvider>(webrtc);
            if (!peer->startSignalingServer(webrtc.signalingPort, webrtc.signalingHost, page)) return nullptr;
            spdlog::info("[ProviderCatalog] WebRTC: open http://{}:{} to talk", webrtc.signalingHost, webrtc.signalingPort);
            return peer;
        });
#endif
}

// The device's microphone and speaker, for a session that opens them itself (providers.audio_input/audio_output).
void registerAudioDevices([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_MINIAUDIO
    registry.add<va::audio::GenericAudioSource, providers::MiniAudioOptions>("miniaudio",
        [](const providers::MiniAudioOptions& options, const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSource> {
            audio::MiniAudioSourceOptions source;
            if (options.has_buffer_seconds()) source.bufferSeconds = std::max(1, options.buffer_seconds());
            return std::make_shared<audio::MiniAudioSource>(source);
        });
    registry.add<va::audio::GenericAudioSink>("miniaudio",
        [](const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSink> { return std::make_shared<audio::MiniAudioSink>(); });
#endif
#ifdef VASDK_HAS_AAUDIO
    registry.add<va::audio::GenericAudioSource, providers::AAudioOptions>("aaudio",
        [](const providers::AAudioOptions& options, const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSource> {
            audio::AAudioSourceOptions source;
            if (options.has_buffer_bytes()) source.bufferBytes = static_cast<size_t>(std::max(0, options.buffer_bytes()));
            return std::make_shared<audio::AAudioSource>(source);
        });
    registry.add<va::audio::GenericAudioSink, providers::AAudioOptions>("aaudio",
        [](const providers::AAudioOptions& options, const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSink> {
            audio::AAudioSinkOptions sink;
            if (options.has_buffer_bytes()) sink.bufferBytes = static_cast<size_t>(std::max(0, options.buffer_bytes()));
            return std::make_shared<audio::AAudioSink>(sink);
        });
#endif
}

#ifdef VASDK_HAS_LLAMACPP
std::optional<va::llm::ChatFormat> chatFormatNamed(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    using va::llm::ChatFormat;
    constexpr std::pair<const char*, ChatFormat> kFormats[] = {
        {"chatml", ChatFormat::CHATML}, {"llama3", ChatFormat::LLAMA3}, {"llama2", ChatFormat::LLAMA2},
        {"mistral", ChatFormat::MISTRAL}, {"gemma", ChatFormat::GEMMA},  {"phi3", ChatFormat::PHI3},
        {"lfm2", ChatFormat::LFM2},       {"lfm2.5", ChatFormat::LFM2_5}, {"lfm2_5", ChatFormat::LFM2_5}};
    for (const auto& [known, format] : kFormats)
        if (name == known) return format;
    return std::nullopt;
}

va::llm::LLMConfig llamaConfig(const providers::LlamaCppOptions& options, const ProviderContext& context) {
    va::llm::LLMConfig config;
    config.model = orEnvironment(options.model_path(), "LLAMACPP_MODEL",
                                 (context.modelsDir + "/LFM2.5-1.2B-Instruct-Q4_K_M.gguf").c_str());
    if (options.has_context_tokens()) config.nCtx = options.context_tokens();
    if (options.has_max_tokens()) config.maxTokens = options.max_tokens();
    if (options.has_temperature()) config.temperature = options.temperature();
    if (options.has_top_p()) config.topP = options.top_p();
    if (options.has_top_k()) config.topK = options.top_k();
    if (options.has_min_p()) config.minP = options.min_p();
    if (options.has_repetition_penalty()) config.repetitionPenalty = options.repetition_penalty();
    if (options.has_threads()) config.nThreads = options.threads();
    if (options.has_gpu_layers()) config.nGpuLayers = options.gpu_layers();
    if (options.has_batch_tokens()) config.nBatch = options.batch_tokens();
    config.nativeLibDir = options.native_library_dir();
    return config;
}
#endif

void registerLlm([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_LLAMACPP
    registry.add<va::llm::LLMTransport, providers::LlamaCppOptions>("llamacpp",
        [](const providers::LlamaCppOptions& options, const ProviderContext& context) -> std::shared_ptr<va::llm::LLMTransport> {
            auto transport = std::make_shared<llm::LlamaCppTransport>(llamaConfig(options, context));
            // The GGUF's chat format renders the tools' descriptions and call syntax: a wrong one
            // gives the model tokens it parrots back.
            const std::string format = orEnvironment(options.chat_format(), "LLAMACPP_CHAT_FORMAT");
            if (auto known = chatFormatNamed(format)) transport->setPromptChatFormat(*known);
            else if (!format.empty()) spdlog::warn("[ProviderCatalog] llamacpp: unknown chat_format \"{}\": the transport's own", format);
            if (options.chunker() == providers::LlamaCppOptions::PUNCTUATION)
                transport->setChunker(options.has_chunker_min_bytes()
                    ? std::make_shared<va::transport::PunctuationChunker>(static_cast<size_t>(std::max(1, options.chunker_min_bytes())))
                    : std::make_shared<va::transport::PunctuationChunker>());
            if (!transport->initialize()) return nullptr;
            return transport;
        });
    registry.add<va::llm::Completion, providers::LlamaCppOptions>("llamacpp",
        [](const providers::LlamaCppOptions& options, const ProviderContext& context) -> std::shared_ptr<va::llm::Completion> {
            if (!options.answer_agent()) return nullptr;
            auto completion = std::make_shared<llm::LlamaCppCompletion>(llamaConfig(options, context));
            return completion->loaded() ? completion : nullptr;
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
    registerAudioDsp(registry);
    registerSpeakerEmbedding(registry);
    registerWebRtc(registry);
    registerAudioDevices(registry);
    registerLlm(registry);
}

} // namespace thirdparty
} // namespace va
