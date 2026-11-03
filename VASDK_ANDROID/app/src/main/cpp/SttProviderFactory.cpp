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

#ifdef VASDK_HAS_CARTESIAPP_STT
#include "CartesiappTranscription/CartesiappTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_FISHAUDIOPP_STT
#include "FishaudioppTranscription/FishaudioppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_GRADIUMPP_STT
#include "GradiumppTranscription/GradiumppTranscriptionProvider.h"
#endif

#ifdef VASDK_HAS_WHISTLE_STT
#include "WhistleSTT/WhistleTranscriptionProvider.hpp"
#endif

#ifdef VASDK_HAS_VOZ_STT
#include "VozSTT/VozTranscriptionProvider.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
#endif

namespace {

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeWhistleStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_WHISTLE_STT)
    auto whistle = std::make_shared<WhistleTranscriptionProvider>(params.modelDir);
    WhistleTranscriptionProvider::InferenceConfig whistleConfig;
    whistleConfig.idleTimeoutMs = 550;
    whistleConfig.partialIntervalMs = 250;
    whistleConfig.minNewSamplesForPartial = params.audioConfig.sampleRate / 4;
    whistleConfig.minNewSamplesForFinal = params.frameSize;
    whistle->setInferenceConfig(whistleConfig);
    whistle->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Whistle STT provider configured (modelDir={})", params.modelDir);
    return whistle;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeVozStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_VOZ_STT)
    // Resolve the Kotlin wrapper here, on the nativeInit (Java) thread: FindClass from the
    // provider's native inference thread would only see the system class loader.
    JNIEnv *env = nullptr;
    if (!params.javaVm ||
        params.javaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        spdlog::error("[VASDK] Voz STT needs a JVM-attached thread");
        return nullptr;
    }
    static jclass recognizerClass = nullptr; // global ref, lives as long as the process
    if (!recognizerClass) {
        jclass localClass = env->FindClass("com/vasdk/android/VozRecognizer");
        if (!localClass) {
            env->ExceptionClear();
            spdlog::error("[VASDK] com.vasdk.android.VozRecognizer not found");
            return nullptr;
        }
        recognizerClass = static_cast<jclass>(env->NewGlobalRef(localClass));
        env->DeleteLocalRef(localClass);
    }
    jmethodID transcribeMethod =
            env->GetStaticMethodID(recognizerClass, "transcribe", "([FI)Ljava/lang/String;");
    if (!transcribeMethod) {
        env->ExceptionClear();
        spdlog::error("[VASDK] VozRecognizer.transcribe not found");
        return nullptr;
    }

    auto recognizer = [javaVm = params.javaVm, transcribeMethod](
            const std::vector<float> &samples, int sampleRate) -> va::audio::TranscriptionResult {
        JNIEnv *threadEnv = nullptr;
        bool didAttach = false;
        const jint status = javaVm->GetEnv(reinterpret_cast<void **>(&threadEnv), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            JavaVMAttachArgs attachArgs{JNI_VERSION_1_6, "vasdk-voz", nullptr};
            if (javaVm->AttachCurrentThread(&threadEnv, &attachArgs) != JNI_OK || !threadEnv)
                throw std::runtime_error("failed to attach JVM thread");
            didAttach = true;
        } else if (status != JNI_OK || !threadEnv) {
            throw std::runtime_error("invalid JVM state");
        }

        std::string json;
        jfloatArray jSamples = threadEnv->NewFloatArray(static_cast<jsize>(samples.size()));
        if (jSamples) {
            threadEnv->SetFloatArrayRegion(jSamples, 0, static_cast<jsize>(samples.size()), samples.data());
            auto jResult = static_cast<jstring>(threadEnv->CallStaticObjectMethod(
                    recognizerClass, transcribeMethod, jSamples, static_cast<jint>(sampleRate)));
            if (jResult) {
                const char *chars = threadEnv->GetStringUTFChars(jResult, nullptr);
                json = chars;
                threadEnv->ReleaseStringUTFChars(jResult, chars);
                threadEnv->DeleteLocalRef(jResult);
            }
            threadEnv->DeleteLocalRef(jSamples);
        }
        if (threadEnv->ExceptionCheck()) threadEnv->ExceptionClear();
        if (didAttach) javaVm->DetachCurrentThread();

        const auto document = nlohmann::json::parse(json, nullptr, false);
        if (document.is_discarded() || !document.is_object())
            throw std::runtime_error("no result from VozRecognizer");
        if (document.contains("error"))
            throw std::runtime_error(document.value("error", std::string{}));

        va::audio::TranscriptionResult result;
        result.text = document.value("text", std::string{});
        for (const auto &item : document.value("words", nlohmann::json::array())) {
            va::audio::TranscriptionResult::Word word;
            word.text = item.value("text", std::string{});
            word.startTimeMs = static_cast<int64_t>(std::llround(item.value("start", 0.0) * 1000.0));
            word.endTimeMs = static_cast<int64_t>(std::llround(item.value("end", 0.0) * 1000.0));
            result.words.push_back(std::move(word));
        }
        return result;
    };

    auto voz = std::make_shared<VozTranscriptionProvider>(recognizer);
    voz->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Voz STT provider configured");
    return voz;
#else
    (void) params;
    return nullptr;
#endif
}

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

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeCartesiaStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_CARTESIAPP_STT)
    auto cartesia = std::make_shared<va::thirdparty::CartesiappTranscriptionProvider>(
            params.cartesiaApiKey,
            cartesiapp::stt::models::INK_WHISPER,
            std::string{},
            params.caBundlePath);
    cartesia->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Cartesia STT provider configured");
    return cartesia;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeFishaudioStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_FISHAUDIOPP_STT)
    auto fishaudio = std::make_shared<va::thirdparty::FishaudioppTranscriptionProvider>(
            params.fishaudioApiKey, std::string{}, params.caBundlePath);
    fishaudio->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Fish Audio STT provider configured");
    return fishaudio;
#else
    (void) params;
    return nullptr;
#endif
}

std::shared_ptr<va::audio::GenericTranscriptionProvider>
makeGradiumStt(const SttProviderParams &params) {
#if defined(VASDK_HAS_GRADIUMPP_STT)
    // GradiumPP has no custom CA bundle knob, so params.caBundlePath is unused here.
    auto gradium = std::make_shared<va::thirdparty::GradiumppTranscriptionProvider>(
            params.gradiumApiKey);
    gradium->setAudioConfiguration(params.audioConfig);
    spdlog::info("[VASDK] Gradium STT provider configured");
    return gradium;
#else
    (void) params;
    return nullptr;
#endif
}

} // namespace

std::shared_ptr<va::audio::GenericTranscriptionProvider>
createTranscriptionProvider(const SttProviderParams &params) {
    std::shared_ptr<va::audio::GenericTranscriptionProvider> provider;
    if (params.providerId == "gradium") {
        provider = makeGradiumStt(params);
    } else if (params.providerId == "soniox") {
        provider = makeSonioxStt(params);
    } else if (params.providerId == "deepgram") {
        provider = makeDeepgramStt(params);
    } else if (params.providerId == "gladia") {
        provider = makeGladiaStt(params);
    } else if (params.providerId == "cartesia") {
        provider = makeCartesiaStt(params);
    } else if (params.providerId == "fishaudio") {
        provider = makeFishaudioStt(params);
    } else if (params.providerId == "whistle") {
        provider = makeWhistleStt(params);
    } else if (params.providerId == "voz") {
        provider = makeVozStt(params);
    } else {
        provider = makeParakeetStt(params);
    }
    if (!provider) {
        // Requested provider isn't compiled in - fall back to whatever is, in priority order.
        provider = makeParakeetStt(params);
        if (!provider) provider = makeGradiumStt(params);
        if (!provider) provider = makeSonioxStt(params);
        if (!provider) provider = makeDeepgramStt(params);
        if (!provider) provider = makeGladiaStt(params);
        if (!provider) provider = makeCartesiaStt(params);
        if (!provider) provider = makeFishaudioStt(params);
    }
    return provider;
}
