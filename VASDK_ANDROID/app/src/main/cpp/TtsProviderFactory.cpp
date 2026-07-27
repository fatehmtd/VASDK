#include "TtsProviderFactory.h"

#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_POCKET_TTS
#include "PocketTTS/PocketVoiceSynthesisProvider.hpp"
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

namespace {

std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>
makePocketTts(const TtsProviderParams &params) {
#if defined(VASDK_HAS_POCKET_TTS)
    auto provider = std::make_shared<PocketVoiceSynthesisProvider>(params.modelDir, "cosette");
    provider->setLsdSteps(4); // 4 steps on Android: ~0.6–0.8 s/chunk
    provider->setAudioConfiguration(params.outputAudioConfig);
    provider->setVoice("jean");
    provider->setPreBufferedSamplesMS(500);
    spdlog::info("[VASDK] Pocket TTS provider configured (modelDir={})", params.modelDir);
    return provider;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>
makeSonioxTts(const TtsProviderParams &params) {
#if defined(VASDK_HAS_SONIOXPP_TTS)
    auto provider = std::make_shared<va::thirdparty::SonioxppVoiceSynthesisProvider>(
            params.sonioxApiKey, "en", "Adrian", params.caBundlePath);
    provider->setAudioConfiguration(params.outputAudioConfig);
    spdlog::info("[VASDK] Soniox TTS provider configured");
    return provider;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>
makeSupertonicTts(const TtsProviderParams &params) {
#if defined(VASDK_HAS_SUPERTONIC_TTS)
    auto provider = std::make_shared<SupertonicVoiceSynthesisProvider>(params.modelDir,
                                                                       params.stylePath);
    provider->setTotalSteps(10); // 10 steps: ~1.5–2.0 s/chunk - good conversational quality
    provider->setAudioConfiguration(params.outputAudioConfig);
    spdlog::info("[VASDK] Supertonic TTS provider configured (modelDir={})", params.modelDir);
    return provider;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>
makeDeepgramTts(const TtsProviderParams &params) {
#if defined(VASDK_HAS_DEEPGRAMPP_TTS)
    auto provider = std::make_shared<va::thirdparty::DeepgramppVoiceSynthesisProvider>(
            params.deepgramApiKey,
            deepgram::speak::models::aura_2::en::THALIA,
            params.caBundlePath);
    provider->setAudioConfiguration(params.outputAudioConfig);
    spdlog::info("[VASDK] Deepgram TTS provider configured");
    return provider;
#else
    (void) params;
    return nullptr;
#endif
}

} // namespace

std::shared_ptr<va::audio::GenericVoiceSynthesisProvider>
createSynthesisProvider(const TtsProviderParams &params) {
    std::shared_ptr<va::audio::GenericVoiceSynthesisProvider> provider;
    if (params.providerId == "soniox") {
        provider = makeSonioxTts(params);
    } else if (params.providerId == "supertonic") {
        provider = makeSupertonicTts(params);
    } else if (params.providerId == "deepgram") {
        provider = makeDeepgramTts(params);
    } else {
        provider = makePocketTts(params);
    }
    if (!provider) {
        // Requested provider isn't compiled in - fall back to whatever is, in priority order.
        provider = makePocketTts(params);
        if (!provider) provider = makeSonioxTts(params);
        if (!provider) provider = makeSupertonicTts(params);
        if (!provider) provider = makeDeepgramTts(params);
    }
    return provider;
}
