#include "SttProviderFactory.h"

#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_PARAKEET
#include "Parakeet/ParakeetTranscriptionProvider.hpp"
#endif

#ifdef VASDK_HAS_SONIOXPP_STT
#include "SonioxppTranscription/SonioxppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_DEEPGRAMPP_STT
#include "DeepgramppTranscription/DeepgramppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_GLADIAPP
#include "GladiappTranscription/GladiaPPTranscriptionProvider.h"
#endif

namespace {

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeParakeetStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_PARAKEET)
    auto parakeet = std::make_shared<ParakeetTranscriptionProvider>(params.modelDir);
    ParakeetTranscriptionProvider::InferenceConfig parakeetConfig;
    parakeetConfig.idleTimeoutMs = 550;
    parakeetConfig.partialIntervalMs = 150;
    parakeetConfig.minNewSamplesForPartial = params.frameSize;
    parakeetConfig.minNewSamplesForFinal = parakeetConfig.minNewSamplesForPartial * 2;
    parakeetConfig.contextSamples = params.audioConfig.sampleRate / 2;
    parakeetConfig.silencePaddingSeconds = 1.0f;

    parakeet->setInferenceConfig(parakeetConfig);
    parakeet->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Parakeet STT provider configured (modelDir={})", params.modelDir);
    return parakeet;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeSonioxStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_SONIOXPP_STT)
    auto soniox = std::make_shared<va::thirdparty::SonioxppTranscriptionProvider>(
            params.sonioxApiKey, params.caBundlePath);
    soniox->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Soniox STT provider configured");
    return soniox;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeDeepgramStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_DEEPGRAMPP_STT)
    auto deepgram = std::make_shared<va::thirdparty::DeepgramppTranscriptionProvider>(
            params.deepgramApiKey,
            deepgram::listen::models::nova_3::GENERAL,
            deepgram::listen::languages::nova_3::MULTILINGUAL,
            params.caBundlePath);
    deepgram->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Deepgram STT provider configured");
    return deepgram;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeGladiaStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_GLADIAPP)
    auto gladia = std::make_shared<va::thirdparty::GladiaPPTranscriptionProvider>(
            params.gladiaApiKey, params.caBundlePath);
    gladia->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Gladia STT provider configured");
    return gladia;
#else
    (void) params;
    return nullptr;
#endif
}

} // namespace

std::shared_ptr<va::audio::GenericTranscriptionProvider>
createTranscriptionProvider(const SttProviderParams &params) {
    std::shared_ptr<va::audio::GenericTranscriptionProvider> provider;
    if (params.providerId == "soniox") {
        provider = makeSonioxStt(params);
    } else if (params.providerId == "deepgram") {
        provider = makeDeepgramStt(params);
    } else if (params.providerId == "gladia") {
        provider = makeGladiaStt(params);
    } else {
        provider = makeParakeetStt(params);
    }
    if (!provider) {
        // Requested provider isn't compiled in - fall back to whatever is, in priority order.
        provider = makeParakeetStt(params);
        if (!provider) provider = makeSonioxStt(params);
        if (!provider) provider = makeDeepgramStt(params);
        if (!provider) provider = makeGladiaStt(params);
    }
    return provider;
}
