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

// Each provider's options struct holds its defaults; a registration fills only what the config
// (or the provider's environment variable) sets.

void registerVad([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SILERO_VAD
    registry.add<Vad, providers::SileroOptions>("silero",
        [](const providers::SileroOptions& options, const ProviderContext& context) -> std::shared_ptr<Vad> {
            vad::SileroVADOptions silero;
            silero.modelPath = options.model_path().empty() ? context.modelsDir + "/silero_vad.onnx" : options.model_path();
            if (options.has_threshold()) silero.threshold = std::clamp(options.threshold(), 0.0f, 1.0f);
            auto detector = std::make_shared<vad::SileroVAD>(silero);
            if (!detector->initialize(context.audio, context.audio.sampleRate * 4)) return nullptr;
            return detector;
        });
#endif
}

// Cloud services first, then local models: the same preference VASDK_APP's #if chain had.
void registerStt([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_STT
    registry.add<Stt, CloudOptions>("gradium", [](const CloudOptions& options, const ProviderContext& context) {
        stt::GradiumppTranscriptionOptions gradium;
        gradium.apiKey = orEnvironment(options.api_key(), "GRADIUM_API_KEY");
        return cloud<stt::GradiumppTranscriptionProvider>(gradium, "GRADIUM_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_SONIOXPP_STT
    registry.add<Stt, CloudOptions>("soniox", [](const CloudOptions& options, const ProviderContext& context) {
        stt::SonioxppTranscriptionOptions soniox;
        soniox.apiKey       = orEnvironment(options.api_key(), "SONIOX_API_KEY");
        soniox.caBundlePath = caBundle(options);
        return cloud<stt::SonioxppTranscriptionProvider>(soniox, "SONIOX_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_STT
    registry.add<Stt, CloudOptions>("deepgram", [](const CloudOptions& options, const ProviderContext& context) {
        stt::DeepgramppTranscriptionOptions deepgram;
        deepgram.apiKey       = orEnvironment(options.api_key(), "DEEPGRAM_API_KEY");
        deepgram.caBundlePath = caBundle(options);
        return cloud<stt::DeepgramppTranscriptionProvider>(deepgram, "DEEPGRAM_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_CARTESIAPP_STT
    registry.add<Stt, CloudOptions>("cartesia", [](const CloudOptions& options, const ProviderContext& context) {
        stt::CartesiappTranscriptionOptions cartesia;
        cartesia.apiKey       = orEnvironment(options.api_key(), "CARTESIA_API_KEY");
        cartesia.caBundlePath = caBundle(options);
        return cloud<stt::CartesiappTranscriptionProvider>(cartesia, "CARTESIA_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_STT
    registry.add<Stt, CloudOptions>("fishaudio", [](const CloudOptions& options, const ProviderContext& context) {
        stt::FishaudioppTranscriptionOptions fishaudio;
        fishaudio.apiKey       = orEnvironment(options.api_key(), "FISHAUDIO_API_KEY");
        fishaudio.caBundlePath = caBundle(options);
        return cloud<stt::FishaudioppTranscriptionProvider>(fishaudio, "FISHAUDIO_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_GLADIAPP
    registry.add<Stt, CloudOptions>("gladia", [](const CloudOptions& options, const ProviderContext& context) {
        stt::GladiaPPTranscriptionOptions gladia;
        gladia.apiKey       = orEnvironment(options.api_key(), "GLADIA_API_KEY");
        gladia.caBundlePath = caBundle(options);
        return cloud<stt::GladiaPPTranscriptionProvider>(gladia, "GLADIA_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_WHISTLE_STT
    registry.add<Stt, LocalSttOptions>("whistle", [](const LocalSttOptions& options, const ProviderContext& context) {
        stt::WhistleTranscriptionOptions whistle;
        whistle.modelPath = modelDir(options.model_dir(), context, "whistle");
        if (options.idle_timeout_ms()) whistle.idleTimeoutMs = options.idle_timeout_ms();
        if (options.partial_interval_ms()) whistle.partialIntervalMs = options.partial_interval_ms();
        whistle.minNewSamplesForPartial = context.audio.sampleRate / 4;
        whistle.minNewSamplesForFinal   = context.frameSize;
        whistle.language = orEnvironment(options.language(), "WHISTLE_LANGUAGE", whistle.language.c_str());
        return configured(std::make_shared<stt::WhistleTranscriptionProvider>(whistle), context);
    });
#endif
#ifdef VASDK_HAS_MOONSHINE_STT
    registry.add<Stt, LocalSttOptions>("moonshine", [](const LocalSttOptions& options, const ProviderContext& context) {
        stt::MoonshineSttOptions moonshine;
        moonshine.modelDir = modelDir(options.model_dir(), context, "moonshine");
        if (options.idle_timeout_ms()) moonshine.idleTimeoutMs = options.idle_timeout_ms();
        if (options.partial_interval_ms()) moonshine.partialIntervalMs = options.partial_interval_ms();
        return configured(std::make_shared<stt::MoonshineSttProvider>(moonshine), context);
    });
#endif
#ifdef VASDK_HAS_PARAKEET
    registry.add<Stt, LocalSttOptions>("parakeet", [](const LocalSttOptions& options, const ProviderContext& context) {
        stt::ParakeetTranscriptionOptions parakeet;
        parakeet.modelDir = modelDir(options.model_dir(), context, "parakeet");
        if (options.idle_timeout_ms()) parakeet.idleTimeoutMs = options.idle_timeout_ms();
        if (options.partial_interval_ms()) parakeet.partialIntervalMs = options.partial_interval_ms();
        parakeet.minNewSamplesForPartial = context.audio.sampleRate / 2;
        parakeet.minNewSamplesForFinal   = parakeet.minNewSamplesForPartial * 2;
        parakeet.contextSamples          = context.audio.sampleRate / 2;
        return configured(std::make_shared<stt::ParakeetTranscriptionProvider>(parakeet), context);
    });
#endif
}

void registerTts([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_GRADIUMPP_TTS
    registry.add<Tts, CloudOptions>("gradium", [](const CloudOptions& options, const ProviderContext& context) {
        tts::GradiumppVoiceSynthesisOptions gradium;
        gradium.apiKey  = orEnvironment(options.api_key(), "GRADIUM_API_KEY");
        gradium.voiceId = orEnvironment(options.voice_id(), "GRADIUM_VOICE_ID");
        return cloud<tts::GradiumppVoiceSynthesisProvider>(gradium, "GRADIUM_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_SONIOXPP_TTS
    registry.add<Tts, CloudOptions>("soniox", [](const CloudOptions& options, const ProviderContext& context) {
        tts::SonioxppVoiceSynthesisOptions soniox;
        soniox.apiKey       = orEnvironment(options.api_key(), "SONIOX_API_KEY");
        soniox.voice        = orEnvironment(options.voice_id(), "SONIOX_VOICE_ID", soniox.voice.c_str());
        soniox.caBundlePath = caBundle(options);
        return cloud<tts::SonioxppVoiceSynthesisProvider>(soniox, "SONIOX_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_SUPERTONIC_TTS
    registry.add<Tts, LocalTtsOptions>("supertonic", [](const LocalTtsOptions& options, const ProviderContext& context) {
        const std::string dir = modelDir(options.model_dir(), context, "supertonic3");
        tts::SupertonicVoiceSynthesisOptions supertonic;
        supertonic.onnxDir        = dir + "/onnx";
        supertonic.voiceStylePath = dir + "/voice_styles/" + orEnvironment(options.voice(), "SUPERTONIC_TTS_VOICE", "M1") + ".json";
        supertonic.language       = orEnvironment(options.language(), "SUPERTONIC_TTS_LANG", supertonic.language.c_str());
        // Diffusion steps trade quality for latency; synthesis time scales roughly linearly with them.
        if (options.steps()) supertonic.totalSteps = options.steps();
        else if (const char* steps = std::getenv("SUPERTONIC_TTS_STEPS")) supertonic.totalSteps = std::atoi(steps);
        return configured(std::make_shared<tts::SupertonicVoiceSynthesisProvider>(supertonic), context);
    });
#endif
#ifdef VASDK_HAS_DEEPGRAMPP_TTS
    registry.add<Tts, CloudOptions>("deepgram", [](const CloudOptions& options, const ProviderContext& context) {
        tts::DeepgramppVoiceSynthesisOptions deepgram;
        deepgram.apiKey       = orEnvironment(options.api_key(), "DEEPGRAM_API_KEY");
        deepgram.caBundlePath = caBundle(options);
        return cloud<tts::DeepgramppVoiceSynthesisProvider>(deepgram, "DEEPGRAM_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_CARTESIAPP_TTS
    registry.add<Tts, CloudOptions>("cartesia", [](const CloudOptions& options, const ProviderContext& context) {
        tts::CartesiappVoiceSynthesisOptions cartesia;
        cartesia.apiKey       = orEnvironment(options.api_key(), "CARTESIA_API_KEY");
        cartesia.voiceId      = orEnvironment(options.voice_id(), "CARTESIA_VOICE_ID");
        cartesia.caBundlePath = caBundle(options);
        return cloud<tts::CartesiappVoiceSynthesisProvider>(cartesia, "CARTESIA_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_FISHAUDIOPP_TTS
    registry.add<Tts, CloudOptions>("fishaudio", [](const CloudOptions& options, const ProviderContext& context) {
        tts::FishaudioppVoiceSynthesisOptions fishaudio;
        fishaudio.apiKey       = orEnvironment(options.api_key(), "FISHAUDIO_API_KEY");
        fishaudio.referenceId  = orEnvironment(options.voice_id(), "FISHAUDIO_VOICE_ID", fishaudio.referenceId.c_str());
        fishaudio.caBundlePath = caBundle(options);
        return cloud<tts::FishaudioppVoiceSynthesisProvider>(fishaudio, "FISHAUDIO_API_KEY", context);
    });
#endif
#ifdef VASDK_HAS_MOSS_TTS
    registry.add<Tts, LocalTtsOptions>("moss", [](const LocalTtsOptions& options, const ProviderContext& context) {
        tts::MossVoiceSynthesisOptions moss;
        moss.modelDir = modelDir(options.model_dir(), context, "moss_tts");
        moss.voice    = orEnvironment(options.voice(), "MOSS_TTS_VOICE", moss.voice.c_str());
        if (options.prebuffer_ms() > 0) moss.prebufferMs = options.prebuffer_ms();
        return configured(std::make_shared<tts::MossVoiceSynthesisProvider>(moss), context);
    });
#endif
#ifdef VASDK_HAS_PHONON_TTS
    registry.add<Tts, LocalTtsOptions>("phonon", [](const LocalTtsOptions& options, const ProviderContext& context) {
        tts::PhononVoiceSynthesisOptions phonon;
        phonon.modelDir = modelDir(options.model_dir(), context, "phonon_tts");
        phonon.voice    = orEnvironment(options.voice(), "PHONON_TTS_VOICE", phonon.voice.c_str());
        phonon.language = orEnvironment(options.language(), "PHONON_TTS_LANG", phonon.language.c_str());
        phonon.normalizationRules = orEnvironment("", "PHONON_TTS_RULES", phonon.normalizationRules.c_str());
        return configured(std::make_shared<tts::PhononVoiceSynthesisProvider>(phonon), context);
    });
#endif
#ifdef VASDK_HAS_POCKET_TTS
    registry.add<Tts, LocalTtsOptions>("pocket", [](const LocalTtsOptions& options, const ProviderContext& context) {
        tts::PocketVoiceSynthesisOptions pocket;
        pocket.modelDir = modelDir(options.model_dir(), context, "pocket_tts");
        pocket.voice    = orEnvironment(options.voice(), "POCKET_TTS_VOICE", pocket.voice.c_str());
        if (options.steps()) pocket.lsdSteps = options.steps();
        if (options.prebuffer_ms() > 0) pocket.prebufferMs = options.prebuffer_ms();
        return configured(std::make_shared<tts::PocketVoiceSynthesisProvider>(pocket), context);
    });
#endif
}

void registerTurn([[maybe_unused]] engine::ProviderRegistry& registry) {
#ifdef VASDK_HAS_SMART_TURN_V3
    registry.add<va::audio::TurnDetectionProvider, providers::SmartTurnOptions>("smartturn-v3",
        [](const providers::SmartTurnOptions& options, const ProviderContext& context)
            -> std::shared_ptr<va::audio::TurnDetectionProvider> {
            turn::SmartTurnV3Options smartTurnOptions;
            smartTurnOptions.modelPath =
                options.model_path().empty() ? context.modelsDir + "/smart_turn_v3.onnx" : options.model_path();
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
    registry.add<va::audio::GenericAudioSource>("miniaudio",
        [](const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSource> { return std::make_shared<audio::MiniAudioSource>(); });
    registry.add<va::audio::GenericAudioSink>("miniaudio",
        [](const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSink> { return std::make_shared<audio::MiniAudioSink>(); });
#endif
#ifdef VASDK_HAS_AAUDIO
    registry.add<va::audio::GenericAudioSource>("aaudio",
        [](const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSource> { return std::make_shared<audio::AAudioSource>(); });
    registry.add<va::audio::GenericAudioSink>("aaudio",
        [](const ProviderContext&) -> std::shared_ptr<va::audio::GenericAudioSink> { return std::make_shared<audio::AAudioSink>(); });
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

