#include "VozProvider.h"

#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_VOZ_STT
#include "VozSTT/VozTranscriptionProvider.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
#endif

void registerVozProvider(va::engine::ProviderRegistry &registry, JavaVM *javaVm) {
#if defined(VASDK_HAS_VOZ_STT)
    registry.add<va::audio::GenericTranscriptionProvider>("voz",
        [javaVm](const va::engine::ProviderContext &context) -> std::shared_ptr<va::audio::GenericTranscriptionProvider> {
        // Resolve the Kotlin wrapper here, on the nativeInit (Java) thread: FindClass from the
        // provider's native inference thread would only see the system class loader.
        JNIEnv *env = nullptr;
        if (!javaVm ||
            javaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
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

        auto recognizer = [javaVm = javaVm, transcribeMethod](
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
            va::thirdparty::VozTranscriptionOptions vozOptions;
            vozOptions.recognizer = recognizer;
            auto voz = std::make_shared<va::thirdparty::VozTranscriptionProvider>(vozOptions);
            voz->setAudioConfiguration(context.audio);
            spdlog::info("[VASDK] Voz STT provider configured");
            return voz;
        });
#else
    (void) registry;
    (void) javaVm;
#endif
}
