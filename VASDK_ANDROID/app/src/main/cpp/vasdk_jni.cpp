#include "JniUtils.h"
#include "ToolRegistration.h"
#include "VasdkSession.h"
#include "VozProvider.h"

#include "ProviderCatalog/ProviderCatalog.h"
#include "vasdk/engine/events.pb.h"

#include <VASDK/LocalTransport.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/llm/PromptTemplate.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include <algorithm>

#ifdef VASDK_HAS_PYANNOTE_SPEAKER
#include "PyannoteSpeaker/PyannoteEmbeddingProvider.h"
#endif

#ifdef VASDK_HAS_LLAMACPP
#include "LlamaCpp/LlamaCppTransport.h"
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <jni.h>
#include <android/log.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#define VASDK_TAG "VASDK"

static JavaVM *gJvm = nullptr;
static std::unique_ptr<VasdkSession> gSession;
static std::mutex gSessionMutex;

//  JNI_OnLoad

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void * /*reserved*/) {
    gJvm = vm;

    // Route all spdlog output through Android logcat
    auto sink = std::make_shared<spdlog::sinks::android_sink_mt>(VASDK_TAG);
    auto logger = std::make_shared<spdlog::logger>("vasdk", sink);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);
    spdlog::info("[VASDK] JNI library loaded");
    return JNI_VERSION_1_6;
}

namespace {

// The JNIEnv of the calling thread, attached to the JVM for as long as the thread lives (the
// session's event thread, a tool's thread).
JNIEnv *attachedEnv() {
    struct Attachment {
        JNIEnv *env = nullptr;
        bool attached = false;
        ~Attachment() {
            if (attached) gJvm->DetachCurrentThread();
        }
    };
    thread_local Attachment attachment;
    if (attachment.env) return attachment.env;
    const jint status = gJvm->GetEnv(reinterpret_cast<void **>(&attachment.env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-native", nullptr};
        if (gJvm->AttachCurrentThread(&attachment.env, &args) != JNI_OK) attachment.env = nullptr;
        else attachment.attached = true;
    } else if (status != JNI_OK) {
        attachment.env = nullptr;
    }
    return attachment.env;
}

// The callbacks VasdkEngine implements, fed from the session's events (on its event thread).
struct Callbacks {
    jweak target = nullptr;
    jmethodID onTranscript = nullptr, onStatus = nullptr, onLlmResponse = nullptr, onError = nullptr;
    std::string replySoFar;   // AgentReply carries the whole reply so far; Kotlin wants the new part

    void call(const std::function<void(JNIEnv *, jobject)> &invoke) const {
        JNIEnv *env = attachedEnv();
        if (!env) return;
        jobject callback = env->NewLocalRef(target);
        if (!callback) return;
        invoke(env, callback);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(callback);
    }
    void status(const std::string &text) const {
        call([&](JNIEnv *env, jobject callback) {
            jstring jText = env->NewStringUTF(text.c_str());
            env->CallVoidMethod(callback, onStatus, jText);
            env->DeleteLocalRef(jText);
        });
    }

    void onEvent(const vasdk::engine::Event &event) {
        using Event = vasdk::engine::Event;
        switch (event.kind_case()) {
        case Event::kTranscript:
            call([&](JNIEnv *env, jobject callback) {
                jstring jText = env->NewStringUTF(event.transcript().text().c_str());
                env->CallVoidMethod(callback, onTranscript, jText, event.transcript().is_final() ? JNI_TRUE : JNI_FALSE);
                env->DeleteLocalRef(jText);
            });
            break;
        case Event::kAgentReply: {
            const auto &reply = event.agent_reply();
            const std::string chunk =
                    reply.text().rfind(replySoFar, 0) == 0 ? reply.text().substr(replySoFar.size()) : reply.text();
            replySoFar = reply.is_final() ? std::string() : reply.text();
            nlohmann::json choices = nlohmann::json::array();
            for (int i = 0; i < reply.choices_size(); ++i)
                choices.push_back({{"text", reply.choices(i)},
                                   {"value", i < reply.choice_values_size() ? reply.choice_values(i) : std::string()}});
            call([&](JNIEnv *env, jobject callback) {
                jstring jText = env->NewStringUTF(chunk.c_str());
                jstring jChoices = env->NewStringUTF(choices.dump().c_str());
                env->CallVoidMethod(callback, onLlmResponse, jText, reply.is_final() ? JNI_TRUE : JNI_FALSE,
                                    reply.expects_reply() ? JNI_TRUE : JNI_FALSE, jChoices);
                env->DeleteLocalRef(jText);
                env->DeleteLocalRef(jChoices);
            });
            break;
        }
        case Event::kAgentState:
            // A new or cut-off reply: whatever was streamed before isn't part of the next one.
            if (event.agent_state().state() == "THINKING" || event.agent_state().state() == "INTERRUPTED") replySoFar.clear();
            // "AGENT_STATE:<state>:<transition>", parsed by MainViewModel.
            status("AGENT_STATE:" + event.agent_state().state() + ":" + event.agent_state().transition());
            break;
        case Event::kUserSpeech: {
            using Speech = vasdk::engine::UserSpeech;
            const auto state = event.user_speech().state();
            status(state == Speech::STARTED ? "SPEECH_STARTING" : state == Speech::ONGOING ? "SPEECH_ONGOING" : "SPEECH_ENDED");
            break;
        }
        case Event::kError: {
            const auto &error = event.error();
            std::string details = error.message() + " | severity=" +
                                  vasdk::engine::Error::Severity_Name(error.severity()).substr(std::string("SEVERITY_").size());
            if (!error.source().empty()) details += " | source=" + error.source();
            call([&](JNIEnv *env, jobject callback) {
                jstring jText = env->NewStringUTF(details.c_str());
                env->CallVoidMethod(callback, onError, static_cast<jint>(error.code()), jText);
                env->DeleteLocalRef(jText);
            });
            break;
        }
        default:
            break;
        }
    }
};

} // namespace

//  nativeInit
//
// engineConfig: a vasdk.engine.EngineConfig in text proto (VasdkEngine builds it from the
// settings): providers, their keys and model folders, audio formats, activation, turn-taking
// and, for a cloud LLM, its endpoint. agentPath: an agent file to answer from before the LLM;
// empty for none. The local LLM (llama.cpp) is set up here: it isn't a config provider yet.

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vasdk_android_VasdkEngine_nativeInit(JNIEnv *env, jobject thiz, jstring engineConfig,
                                              jstring agentPath, jstring nativeLibDir,
                                              jstring speakerModelPath, jboolean useLocalLLM,
                                              jstring llmModelPath, jstring llmChatFormat,
                                              jstring systemPrompt,
                                              jstring responseFormatInstruction,
                                              jstring toolCallExceptionClause) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        spdlog::warn("[VASDK] nativeInit: session already exists");
        return JNI_TRUE;
    }

    vasdk::engine::EngineConfig config;
    std::string error;
    if (!va::engine::parseProto(jstringToStdString(env, engineConfig), va::engine::ProtoFormat::Text, config, &error)) {
        spdlog::error("[VASDK] Engine config: {}", error);
        return JNI_FALSE;
    }

    auto session = std::make_unique<VasdkSession>();
    session->callbackRef = env->NewWeakGlobalRef(thiz);
    auto fail = [&](const std::string &message) {
        spdlog::error("[VASDK] {}", message);
        env->DeleteWeakGlobalRef(session->callbackRef);
        return JNI_FALSE;
    };

    jclass cls = env->GetObjectClass(thiz);
    auto callbacks = std::make_shared<Callbacks>();
    callbacks->target = session->callbackRef;
    callbacks->onTranscript = env->GetMethodID(cls, "onTranscriptReceived", "(Ljava/lang/String;Z)V");
    callbacks->onStatus = env->GetMethodID(cls, "onStatusChanged", "(Ljava/lang/String;)V");
    callbacks->onLlmResponse = env->GetMethodID(cls, "onLlmResponse", "(Ljava/lang/String;ZZLjava/lang/String;)V");
    callbacks->onError = env->GetMethodID(cls, "onError", "(ILjava/lang/String;)V");
    jmethodID onToolCall = env->GetMethodID(cls, "onToolCall", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!callbacks->onTranscript || !callbacks->onStatus || !callbacks->onLlmResponse || !callbacks->onError || !onToolCall)
        return fail("Could not resolve callback methods on VasdkEngine");
#ifdef VASDK_HAS_VIZANIM
    jmethodID onAudioFeatures = env->GetMethodID(cls, "onAudioFeatures", "(Ljava/lang/String;[F)V");
    if (!onAudioFeatures) return fail("Could not resolve onAudioFeatures on VasdkEngine");
#endif

    //  Providers, by the ids the config names
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    registerVozProvider(registry, gJvm);
    va::engine::Runtime::Options runtimeOptions;
    if (!va::engine::applyAudioConfig(config.audio(), runtimeOptions.inputAudio, runtimeOptions.outputAudio,
                                      runtimeOptions.frameSize, &error))
        return fail("Audio config: " + error);
    session->runtime = va::engine::Runtime::create(config, registry, runtimeOptions, &error);
    if (!session->runtime) return fail("Engine: " + error);

    //  The local LLM (llama.cpp); a cloud one comes from the config's llm block
    const std::string systemPromptText = jstringToStdString(env, systemPrompt);
    session->baseSystemPrompt = systemPromptText;
    session->useLocalLLM = useLocalLLM == JNI_TRUE;
    std::shared_ptr<va::llm::LLMTransport> localLlm;
#ifdef VASDK_HAS_LLAMACPP
    if (session->useLocalLLM) {
        va::llm::LLMConfig localConfig;
        localConfig.apiKey = "sk-test";
        localConfig.model = jstringToStdString(env, llmModelPath);
        localConfig.systemPrompt = systemPromptText;
        localConfig.temperature = 0.1f;
        localConfig.minP = 0.15f;
        localConfig.maxTokens = 512;
        localConfig.nCtx = 48000;
        localConfig.nativeLibDir = jstringToStdString(env, nativeLibDir);
        localConfig.streaming = false;
        localConfig.topK = 50;
        localConfig.repetitionPenalty = 1.05f;
        auto llama = std::make_shared<va::transport::LlamaCppTransport>(localConfig);
        llama->setChunker(std::make_shared<va::transport::PunctuationChunker>(10));
        // The model's chat format drives how tool descriptions and call syntax are rendered, so it
        // must match the GGUF: LFM2.5 tool tokens fed to Gemma are garbage the model parrots back.
        const std::string chatFormat = jstringToStdString(env, llmChatFormat);
        if (auto format = chatFormatFromString(chatFormat)) llama->setPromptChatFormat(*format);
        else spdlog::warn("[VASDK] Unknown or missing llmChatFormat \"{}\": the transport's default", chatFormat);
        if (!llama->initialize()) return fail("Failed to initialize the local LLM");
        localLlm = std::move(llama);
    }
#else
    if (session->useLocalLLM) return fail("This build has no local LLM (llama.cpp)");
    (void) llmModelPath;
    (void) llmChatFormat;
    (void) nativeLibDir;
#endif

    //  The session: mic and speaker over AAudio
    session->audioSource = std::make_shared<va::android::AAudioSource>();
    session->audioSink = std::make_shared<va::android::AAudioSink>();
    va::engine::Session::Options options;
    options.transport = std::make_shared<va::transport::LocalTransport>(session->audioSource, session->audioSink);
    options.systemPrompt = systemPromptText;
    // Every session registers tools below: the reply format allows tool calls.
    options.responseFormatInstruction = jstringToStdString(env, responseFormatInstruction) +
                                        jstringToStdString(env, toolCallExceptionClause);
    options.llmTransport = localLlm;
    const std::string agentFile = jstringToStdString(env, agentPath);
    if (!agentFile.empty()) {
        std::vector<std::string> warnings;
        options.agent = va::flow::CompiledAgent::load(agentFile, &error, &warnings);
        if (!options.agent) return fail("Agent " + agentFile + ": " + error);
        for (const auto &warning : warnings) spdlog::warn("[VASDK] Agent: {}", warning);
    }
    session->engine = session->runtime->createSession(std::move(options), &error);
    if (!session->engine) return fail("Session: " + error);
    session->llmTransport = session->engine->llmTransport();
    session->toolDispatcher = session->engine->toolDispatcher();

#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    // Optional: logs SAME/NEW speaker per utterance; skipped when the model is missing.
    const std::string speakerModel = jstringToStdString(env, speakerModelPath);
    if (!speakerModel.empty()) {
        auto speakerProvider = std::make_shared<va::thirdparty::PyannoteEmbeddingProvider>(speakerModel);
        if (speakerProvider->initialize(runtimeOptions.inputAudio)) {
            session->speakerIdProcessor = std::make_shared<va::pipeline::SpeakerIdentificationProcessor>(speakerProvider);
            session->speakerIdProcessor->setRingBuffer(session->engine->inputRingBuffer());
            session->engine->addProcessor(va::engine::Session::Slot::AfterTranscription, session->speakerIdProcessor);
        } else {
            spdlog::error("[VASDK] Failed to initialize PyannoteEmbeddingProvider - speaker tracking disabled");
        }
    }
#else
    (void) speakerModelPath;
#endif

#ifdef VASDK_HAS_VIZANIM
    // MIC: AudioFrames → AudioFeaturesFrame → JNI, right after the mic.
    session->micAnimProcessor = std::make_shared<va::thirdparty::AudioInputVizAnimFrameProcessor>(
            session->engine->inputRingBuffer(), runtimeOptions.inputAudio, runtimeOptions.inputAudio.sampleRate * 10);
    session->audioFeaturesProcessor = std::make_shared<AudioFeatureSinkProcessor>();
    session->audioFeaturesProcessor->setCallbackTarget(gJvm, session->callbackRef, onAudioFeatures);
    session->engine->addProcessor(va::engine::Session::Slot::AfterInput, session->micAnimProcessor);
    session->engine->addProcessor(va::engine::Session::Slot::AfterInput, session->audioFeaturesProcessor);
    // SYNTH: features extracted when bytes actually reach the speaker.
    session->synthAudioObserver = std::make_shared<SynthAudioObserver>();
    if (!session->synthAudioObserver->initialize(runtimeOptions.outputAudio, gJvm, session->callbackRef, onAudioFeatures))
        spdlog::warn("[VASDK] SynthAudioObserver failed to initialize VizAnimProvider - SYNTH animation disabled");
    session->audioSink->setPlaybackCallback(
            [observer = std::weak_ptr<SynthAudioObserver>(session->synthAudioObserver)](const char *data, size_t bytes) {
                if (auto o = observer.lock()) o->onPlayback(data, bytes);
            });
#endif

    //  Tools: C++ handlers that call Android APIs through VasdkEngine.onToolCall
    AndroidToolBridge androidToolBridge = [ref = session->callbackRef, onToolCall](
            const std::string &toolName, const std::string &argsJson) -> std::string {
        JNIEnv *toolEnv = attachedEnv();
        if (!toolEnv) return R"({"error":"failed to attach JVM thread"})";
        std::string result = R"({"error":"callback unavailable"})";
        jobject callback = toolEnv->NewLocalRef(ref);
        if (!callback) return result;
        jstring jName = toolEnv->NewStringUTF(toolName.c_str());
        jstring jArgs = toolEnv->NewStringUTF(argsJson.c_str());
        auto jResult = static_cast<jstring>(toolEnv->CallObjectMethod(callback, onToolCall, jName, jArgs));
        if (toolEnv->ExceptionCheck()) toolEnv->ExceptionClear();
        toolEnv->DeleteLocalRef(jName);
        toolEnv->DeleteLocalRef(jArgs);
        toolEnv->DeleteLocalRef(callback);
        if (jResult) {
            const char *chars = toolEnv->GetStringUTFChars(jResult, nullptr);
            result = chars;
            toolEnv->ReleaseStringUTFChars(jResult, chars);
            toolEnv->DeleteLocalRef(jResult);
        }
        return result;
    };
    // HINT: the more tools you add the less likely they'll be called, a limit of small models.
    registerCommunicationTools(session.get(), androidToolBridge);
    registerNotificationAndAlarmTools(session.get(), androidToolBridge);
    registerCalendarTools(session.get(), androidToolBridge);
    registerCameraTools(session.get(), androidToolBridge);

    if (session->useLocalLLM && session->llmTransport) {
        // llama.cpp renders requests with the GGUF's own template: the tool descriptions go into
        // the system prompt. Where the tool prompt carries the whole response contract (formats
        // with no tool-call tags, and LFM2/LFM2.5's native tool tokens), a trailing text-shaped
        // format instruction contradicts it and small models stop calling tools.
        const auto format = session->llmTransport->getPromptChatFormat();
        const std::string toolDescriptions = session->toolDispatcher->renderToolDescriptions(format);
        va::llm::LLMConfig updated = session->llmTransport->getConfig();
        updated.systemPrompt = toolDescriptions.empty() ? systemPromptText : systemPromptText + "\n\n" + toolDescriptions;
        session->llmTransport->setConfig(updated);
        const bool toolPromptOwnsContract =
                !toolDescriptions.empty() &&
                (va::llm::PromptTemplate::create(format)->toolCallTags().first.empty() ||
                 format == va::llm::ChatFormat::LFM2_5 || format == va::llm::ChatFormat::LFM2);
        if (toolPromptOwnsContract) session->llmTransport->setResponseFormatInstruction({});
    }
    spdlog::info("[VASDK] {} tools", session->toolDispatcher->getToolNames().size());

    session->engine->addListener([callbacks](const vasdk::engine::Event &event) { callbacks->onEvent(event); });
    spdlog::info("[VASDK] {}", session->engine->describe());
    gSession = std::move(session);
    return JNI_TRUE;
}

//  nativeStart

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vasdk_android_VasdkEngine_nativeStart(JNIEnv * /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) {
        spdlog::error("[VASDK] nativeStart: call nativeInit first");
        return JNI_FALSE;
    }
    std::string error;
    if (!gSession->engine->start(&error)) {
        spdlog::error("[VASDK] Failed to start: {}", error);
        return JNI_FALSE;
    }
    spdlog::info("[VASDK] Pipeline started");
    return JNI_TRUE;
}

//  nativeStop

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeStop(JNIEnv * /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;
    gSession->engine->stop();
    spdlog::info("[VASDK] Pipeline stopped");
}

//  nativeRelease

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeRelease(JNIEnv *env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;
    gSession->engine->stop();
    gSession->engine.reset();   // events stop before the callback target goes
    if (gSession->callbackRef) {
        env->DeleteWeakGlobalRef(gSession->callbackRef);
        gSession->callbackRef = nullptr;
    }
    gSession.reset();
    spdlog::info("[VASDK] Session released");
}

// The UI's cancel-speech button: cuts the reply, as a barge-in would.
extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeTrigger(JNIEnv * /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) gSession->engine->interrupt();
}

//  nativeSetBackchannelConfig

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeSetBackchannelConfig(JNIEnv * /*env*/, jobject /*thiz*/,
                                                              jboolean enabled, jint pendingPlaybackMode,
                                                              jint historyMode, jint tailOverlapMode,
                                                              jfloat duckGainDecibels, jint duckDurationMs) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;
    // Ordinals of the Kotlin enums in VasdkEngine, which mirror the C++ enums' order.
    auto backchannel = gSession->engine->backchannel();
    backchannel.enabled = enabled == JNI_TRUE;
    backchannel.pendingPlaybackMode = static_cast<va::transport::PendingPlaybackMode>(std::clamp<jint>(pendingPlaybackMode, 0, 2));
    backchannel.historyMode = static_cast<va::pipeline::BackchannelHistoryMode>(std::clamp<jint>(historyMode, 0, 1));
    backchannel.tailOverlapMode = static_cast<va::pipeline::TailOverlapMode>(std::clamp<jint>(tailOverlapMode, 0, 1));
    backchannel.duckGainDecibels = std::clamp<jfloat>(duckGainDecibels, -60.0f, 0.0f);
    backchannel.duckDurationMs = static_cast<uint32_t>(std::max<jint>(0, duckDurationMs));
    gSession->engine->setBackchannel(backchannel);
    spdlog::info("[VASDK] Back-channel handling {} (playback {}, history {}, tail overlap {}, duck {} dB for {} ms)",
                 backchannel.enabled ? "on" : "off", pendingPlaybackMode, historyMode, tailOverlapMode,
                 backchannel.duckGainDecibels, backchannel.duckDurationMs);
}

//  nativeFireTrigger: one of the agent's triggers (an app event, a notification), payload as JSON

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeFireTrigger(JNIEnv *env, jobject /*thiz*/, jstring name, jstring payloadJson) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;
    google::protobuf::Struct payload;
    const std::string json = jstringToStdString(env, payloadJson);
    if (!json.empty() && !google::protobuf::util::JsonStringToMessage(json, &payload).ok()) {
        spdlog::error("[VASDK] fireTrigger: the payload isn't a JSON object");
        return;
    }
    gSession->engine->fireTrigger(jstringToStdString(env, name), payload);
}

//  nativeSetNetworkAvailable: down, the local fallbacks speak and listen and the LLM rests

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeSetNetworkAvailable(JNIEnv * /*env*/, jobject /*thiz*/, jboolean available) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) gSession->engine->setNetworkAvailable(available == JNI_TRUE);
}

//  nativeResetConversation

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeResetConversation(JNIEnv * /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) gSession->engine->resetConversation();
}

//  nativeGetPromptPreview

extern "C" JNIEXPORT jstring JNICALL
Java_com_vasdk_android_VasdkEngine_nativeGetPromptPreview(JNIEnv *env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession || !gSession->llmTransport) return env->NewStringUTF("");

    std::string rawPrompt;
    if (gSession->useLocalLLM) {
        // llama.cpp renders the request with the GGUF's own template: show an equivalent built
        // from the same system prompt (tool descriptions already baked in, see nativeInit).
        auto preview = va::llm::PromptTemplate::create(gSession->llmTransport->getPromptChatFormat());
        preview->addSystem(gSession->llmTransport->getConfig().systemPrompt);
        rawPrompt = preview->render();
    } else {
        rawPrompt = gSession->llmTransport->previewRequestPayload();
        const auto parsed = nlohmann::json::parse(rawPrompt, nullptr, false);
        if (!parsed.is_discarded()) rawPrompt = parsed.dump(2);
    }

    nlohmann::json info;
    info["isLocal"] = gSession->useLocalLLM;
    info["systemPrompt"] = gSession->baseSystemPrompt;
    info["tools"] = gSession->toolDispatcher ? gSession->toolDispatcher->getEnabledToolsJson() : nlohmann::json::array();
    info["rawPrompt"] = rawPrompt;
    return env->NewStringUTF(info.dump().c_str());
}
