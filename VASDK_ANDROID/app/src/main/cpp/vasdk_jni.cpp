#include "JniUtils.h"
#include "SttProviderFactory.h"
#include "ToolRegistration.h"
#include "TtsProviderFactory.h"
#include "VasdkBridgeProcessor.h"
#include "VasdkSession.h"

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/TransportInputProcessor.h>
#include <VASDK/TransportOutputProcessor.h>
#include <VASDK/llm/OpenAITransport.h>
#include <VASDK/llm/PromptTemplate.h>
#include <nlohmann/json.hpp>

// VAD provider
#ifdef VASDK_HAS_SILERO_VAD
#include "SileroVAD/SileroVAD.h"
#endif

// Turn detection provider
#ifdef VASDK_HAS_SMART_TURN_V3
#include "SmartTurnV3/SmartTurnV3Provider.h"
#endif

// Llama cpp LLM integration
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

//  nativeInit

extern "C" JNIEXPORT jboolean JNICALL
Java_com_vasdk_android_VasdkEngine_nativeInit(JNIEnv *env, jobject thiz, jstring vadModelPath,
                                              jstring turnDetectionModelPath,
                                              jstring speakerModelPath, jstring nativeLibDir,
                                              jstring sttProviderId, jboolean sttStreaming,
                                              jstring sttModelDir,
                                              jstring llmModelPath, jstring llmChatFormat,
                                              jstring ttsProviderId,
                                              jstring ttsModelDir, jstring ttsStylePath,
                                              jboolean useLocalLLM, jstring llmApiKey,
                                              jstring llmEndpoint, jstring llmModel,
                                              jstring systemPrompt,
                                              jstring responseFormatInstruction,
                                              jstring toolCallExceptionClause,
                                              jstring sonioxApiKey,
                                              jstring deepgramApiKey,
                                              jstring gladiaApiKey,
                                              jstring cartesiaApiKey,
                                              jstring cartesiaVoiceId,
                                              jstring fishaudioApiKey,
                                              jstring fishaudioVoiceId,
                                              jstring gradiumApiKey,
                                              jstring gradiumVoiceId,
                                              jstring caBundlePath) {
    std::lock_guard<std::mutex> lock(gSessionMutex);

    if (gSession) {
        spdlog::warn("[VASDK] nativeInit: session already exists");
        return JNI_TRUE;
    }

    // mbedTLS (sonioxpp/deepgrampp/gladiapp's TLS backend on Android) has no
    // visibility into the system trust store, so every WSS/HTTPS handshake fails
    // cert verification unless we hand it a CA bundle explicitly. VasdkEngine
    // extracts one from Android's TrustManagerFactory at startup and passes the
    // file path here; each provider forwards it to its underlying client(s).
    const std::string caBundlePathStr = jstringToStdString(env, caBundlePath);
    if (caBundlePathStr.empty()) {
        spdlog::warn("[VASDK] nativeInit: no CA bundle path provided; Soniox/Deepgram/Gladia/Cartesia/Fish Audio TLS connections will fail");
    }

    const std::string sonioxApiKeyStr = jstringToStdString(env, sonioxApiKey);
    const std::string deepgramApiKeyStr = jstringToStdString(env, deepgramApiKey);
    const std::string gladiaApiKeyStr = jstringToStdString(env, gladiaApiKey);
    const std::string cartesiaApiKeyStr = jstringToStdString(env, cartesiaApiKey);
    const std::string cartesiaVoiceIdStr = jstringToStdString(env, cartesiaVoiceId);
    const std::string fishaudioApiKeyStr = jstringToStdString(env, fishaudioApiKey);
    const std::string fishaudioVoiceIdStr = jstringToStdString(env, fishaudioVoiceId);
    const std::string gradiumApiKeyStr = jstringToStdString(env, gradiumApiKey);
    const std::string gradiumVoiceIdStr = jstringToStdString(env, gradiumVoiceId);

    auto session = std::make_unique<VasdkSession>();
    session->callbackRef = env->NewWeakGlobalRef(thiz);

    // Resolve Java callback method IDs on VasdkEngine
    jclass cls = env->GetObjectClass(thiz);
    jmethodID onTranscript = env->GetMethodID(cls, "onTranscriptReceived",
                                              "(Ljava/lang/String;Z)V");
    jmethodID onStatus = env->GetMethodID(cls, "onStatusChanged", "(Ljava/lang/String;)V");
    jmethodID onLlmResponse = env->GetMethodID(cls, "onLlmResponse",
                                               "(Ljava/lang/String;ZZLjava/lang/String;)V");
    jmethodID onError = env->GetMethodID(cls, "onError", "(ILjava/lang/String;)V");

    jmethodID onToolCall = env->GetMethodID(cls, "onToolCall",
                                            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    if (!onTranscript || !onStatus || !onLlmResponse || !onError || !onToolCall) {
        spdlog::error("[VASDK] Could not resolve callback methods on VasdkEngine");
        env->DeleteWeakGlobalRef(session->callbackRef);
        return JNI_FALSE;
    }

#ifdef VASDK_HAS_VIZANIM
    jmethodID onAudioFeatures = env->GetMethodID(cls, "onAudioFeatures", "(Ljava/lang/String;[F)V");
    if (!onAudioFeatures) {
        spdlog::error("[VASDK] Could not resolve onAudioFeatures on VasdkEngine");
        env->DeleteWeakGlobalRef(session->callbackRef);
        return JNI_FALSE;
    }
#endif

    // 16 kHz, mono, 16-bit - matches Parakeet / Silero VAD expectations
    va::audio::AudioConfig audioConfig;
    audioConfig.sampleRate = 16000;
    audioConfig.channels = 1;
    audioConfig.format = va::audio::AudioFormat::SHORT;

    // 24 kHz output - matches PocketTTS native rate, avoiding unnecessary resampling
    va::audio::AudioConfig outputAudioConfig;
    outputAudioConfig.sampleRate = 24000;
    outputAudioConfig.channels = 1;
    outputAudioConfig.format = va::audio::AudioFormat::SHORT;

    const int frameSize = audioConfig.sampleRate / 8; // 125 ms frames

    session->audioSource = std::make_shared<va::android::AAudioSource>();
    session->audioSink = std::make_shared<va::android::AAudioSink>();

    auto audioRingBuffer = std::make_shared<va::AudioRingBuffer>(
            static_cast<long>(audioConfig.sampleRate * 90),  // 90-second ring buffer
            audioConfig.getSampleSizeInBytes());

    session->transport = std::make_shared<va::transport::LocalTransport>(session->audioSource,
                                                                         session->audioSink);
    session->transport->setInputAudioConfig(audioConfig);
    session->transport->setOutputAudioConfig(outputAudioConfig);
    session->transport->setFrameSize(frameSize);
    session->transport->setRingBuffer(audioRingBuffer);

    if (!session->transport->initialize()) {
        spdlog::error("[VASDK] Failed to initialize LocalTransport");
        env->DeleteWeakGlobalRef(session->callbackRef);
        return JNI_FALSE;
    }

    auto vadProvider = std::make_shared<SileroVAD>(jstringToStdString(env, vadModelPath));

    if (!vadProvider->initialize(audioConfig, audioConfig.sampleRate * 4)) {
        return JNI_FALSE;
    }

    spdlog::info("[VASDK] VAD provider initialized");

    // Configure VAD parameters
    va::pipeline::VADConfig vadConfig;
    vadConfig.vadThresholdMs = 250;          // Time window for considering voice as detected
    vadConfig.minSpeechDurationMs = 250;     // Minimum speech duration to confirm speech presence
    vadConfig.maxSpeechPauseDurationMs = 500; // Max pause before considering speech ended
    vadConfig.maxSpeechEndDurationMs = 250;  // Max time in ending state before fully ended

    // Lookback 5x frame (~500 ms before the detecting frame) so soft word onsets that precede
    // Silero's trigger still reach the STT.
    session->vadFrameProcessor = std::make_shared<va::pipeline::VADFrameProcessor>(vadProvider, 5.0f);
    session->vadFrameProcessor->setMinFrameSize(frameSize);
    session->vadFrameProcessor->setConfig(vadConfig);
    session->vadFrameProcessor->setRingBuffer(audioRingBuffer);

    // Configure turn detection parameters
    auto turnDetectionProvider = std::make_shared<va::thirdparty::SmartTurnV3Provider>( /* modelPath= */
            jstringToStdString(env, turnDetectionModelPath));

    if (!turnDetectionProvider->initialize(audioConfig, audioRingBuffer)) {
        return JNI_FALSE;
    }

    session->turnDetectionFrameProcessor = std::make_shared<va::pipeline::TurnDetectionProcessor>(/* provider = */
            turnDetectionProvider);

#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    // Optional: logs SAME/NEW speaker per utterance; skipped when the model is missing.
    const std::string speakerModelPathStr = jstringToStdString(env, speakerModelPath);
    if (!speakerModelPathStr.empty()) {
        auto speakerProvider = std::make_shared<va::thirdparty::PyannoteEmbeddingProvider>(speakerModelPathStr);
        if (speakerProvider->initialize(audioConfig)) {
            session->speakerIdProcessor = std::make_shared<va::pipeline::SpeakerIdentificationProcessor>(speakerProvider);
            session->speakerIdProcessor->setRingBuffer(audioRingBuffer);
        } else {
            spdlog::error("[VASDK] Failed to initialize PyannoteEmbeddingProvider - speaker tracking disabled");
        }
    }
#else
    (void) speakerModelPath;
#endif

    // Configure transcription parameters - the provider is chosen at runtime (see
    // VasdkEngine's Settings UI / sttProviderId), falling back to whichever STT
    // provider is actually compiled in when the requested one isn't available.
    SttProviderParams sttParams;
    sttParams.providerId = jstringToStdString(env, sttProviderId);
    sttParams.modelDir = jstringToStdString(env, sttModelDir);
    sttParams.sonioxApiKey = sonioxApiKeyStr;
    sttParams.deepgramApiKey = deepgramApiKeyStr;
    sttParams.gladiaApiKey = gladiaApiKeyStr;
    sttParams.cartesiaApiKey = cartesiaApiKeyStr;
    sttParams.fishaudioApiKey = fishaudioApiKeyStr;
    sttParams.gradiumApiKey = gradiumApiKeyStr;
    sttParams.caBundlePath = caBundlePathStr;
    sttParams.audioConfig = audioConfig;
    sttParams.frameSize = frameSize;

    auto transcriptionProvider = createTranscriptionProvider(sttParams);
    if (!transcriptionProvider) {
        spdlog::error("[VASDK] No STT provider available (requested=\"{}\")", sttParams.providerId);
        return JNI_FALSE;
    }

    if (!transcriptionProvider->initializeTranscriptionProvider()) {
        return JNI_FALSE;
    }

    session->transcriptionProcessor = std::make_shared<va::pipeline::TranscriptionFrameProcessor>(
            transcriptionProvider);
    session->transcriptionProcessor->setRingBuffer(audioRingBuffer);
    // Gladia's provider only implements the incremental writeAudioData() path (its v2 live
    // API is websocket-streaming only, no batch transcribe-full-segment call), so non-streaming
    // mode - which calls writeFullAudioData() - isn't supported regardless of the UI toggle.
    bool effectiveStreaming = sttStreaming || sttParams.providerId == "gladia";
    if (sttParams.providerId == "gladia" && !sttStreaming) {
        spdlog::warn("[VASDK] Gladia STT does not support non-streaming mode; forcing streaming on");
    }
    session->transcriptionProcessor->setStreamingMode(effectiveStreaming);

    // Prompt text (system prompt, response format instruction, tool-call exception clause)
    // lives in Android string resources (see strings.xml) rather than hardcoded here, so it
    // can be edited/translated without touching native code.
    const std::string systemPromptStr = jstringToStdString(env, systemPrompt);
    const std::string formatInstruction = jstringToStdString(env, responseFormatInstruction);
    const std::string toolCallExceptionClauseStr = jstringToStdString(env, toolCallExceptionClause);

    session->baseSystemPrompt = systemPromptStr;

    std::shared_ptr<va::llm::LLMTransport> llmTransport;
    if (useLocalLLM) {
        // LLM transport initialization
        va::llm::LLMConfig localLLMConfig;
        localLLMConfig.apiKey = "sk-test";
        localLLMConfig.model = jstringToStdString(env, llmModelPath);
        localLLMConfig.systemPrompt = systemPromptStr;
        localLLMConfig.temperature = 0.1f;
        localLLMConfig.minP = 0.15f;
        localLLMConfig.maxTokens = 512;
        localLLMConfig.nCtx = 48000;
        localLLMConfig.nativeLibDir = jstringToStdString(env, nativeLibDir);
        localLLMConfig.streaming = false;
        localLLMConfig.topK = 50;
        localLLMConfig.repetitionPenalty = 1.05f;

        auto llamacppTransport = std::make_shared<va::transport::LlamaCppTransport>(localLLMConfig);
        //llamacppTransport->setChunker(std::make_shared<va::transport::WordCountChunker>(1));
        //llamacppTransport->setChunker(std::make_shared<va::transport::NoChunkingChunker>());
        llamacppTransport->setChunker(std::make_shared<va::transport::PunctuationChunker>(10));

        // The caller declares the model's chat format (VasdkEngine.localLLMChatFormat).
        // It drives how auxiliary prompt text (tool descriptions, call syntax) is
        // rendered, so it must match the GGUF being loaded - e.g. LFM2.5 tool
        // special-tokens fed to Gemma are literal garbage the model parrots back.
        const std::string chatFormatStr = jstringToStdString(env, llmChatFormat);
        if (auto fmt = chatFormatFromString(chatFormatStr)) {
            llamacppTransport->setPromptChatFormat(*fmt);
            spdlog::info("[VASDK] Local LLM chat format set to \"{}\"", chatFormatStr);
        } else if (!chatFormatStr.empty()) {
            spdlog::error(
                    "[VASDK] Unknown llmChatFormat \"{}\" - falling back to transport default",
                    chatFormatStr);
        } else {
            spdlog::warn("[VASDK] llmChatFormat not provided - using transport default");
        }

        llmTransport = std::move(llamacppTransport);
    } else {
        va::llm::LLMConfig remoteLLMConfig;
        remoteLLMConfig.apiKey = jstringToStdString(env, llmApiKey);
        remoteLLMConfig.endpoint = jstringToStdString(env, llmEndpoint);
        remoteLLMConfig.model = jstringToStdString(env, llmModel);
        remoteLLMConfig.systemPrompt = systemPromptStr;
        remoteLLMConfig.temperature = 0.1f;
        remoteLLMConfig.maxTokens = 800;
        remoteLLMConfig.nCtx = 4096;
        auto openAiTransport = std::make_shared<va::llm::OpenAITransport>(remoteLLMConfig);
        llmTransport = std::move(openAiTransport);
    }

    if (!llmTransport->initialize()) {
        spdlog::error("[VASDK] Failed to initialize LLM transport");
        env->DeleteWeakGlobalRef(session->callbackRef);
        return JNI_FALSE;
    }

    session->llmTransport = llmTransport;
    session->useLocalLLM = useLocalLLM;
    session->llmProcessor = std::make_shared<va::llm::LLMProcessor>(llmTransport);
    session->llmProcessor->setHistoryEnabled(true);
    session->llmProcessor->setProcessPartialTranscriptions(false);
    session->llmProcessor->setMaxHistoryLength(useLocalLLM ? 40 : 100);
    session->llmProcessor->setConfidenceThreshold(0.1f);
    session->llmProcessor->setFinalWatchdogTimeoutMs(1000000);
    session->llmProcessor->setFirePolicy(std::make_shared<va::llm::FinalOnlyFirePolicy>());

    // TTS processor - the provider is chosen at runtime (see VasdkEngine's Settings UI /
    // ttsProviderId), falling back to whichever TTS provider is actually compiled in when
    // the requested one isn't available.
    TtsProviderParams ttsParams;
    ttsParams.providerId = jstringToStdString(env, ttsProviderId);
    ttsParams.modelDir = jstringToStdString(env, ttsModelDir);
    ttsParams.stylePath = jstringToStdString(env, ttsStylePath);
    ttsParams.sonioxApiKey = sonioxApiKeyStr;
    ttsParams.deepgramApiKey = deepgramApiKeyStr;
    ttsParams.cartesiaApiKey = cartesiaApiKeyStr;
    ttsParams.cartesiaVoiceId = cartesiaVoiceIdStr;
    ttsParams.fishaudioApiKey = fishaudioApiKeyStr;
    ttsParams.fishaudioVoiceId = fishaudioVoiceIdStr;
    ttsParams.gradiumApiKey = gradiumApiKeyStr;
    ttsParams.gradiumVoiceId = gradiumVoiceIdStr;
    ttsParams.caBundlePath = caBundlePathStr;
    ttsParams.outputAudioConfig = outputAudioConfig;

    auto synthesisProvider = createSynthesisProvider(ttsParams);

    if (synthesisProvider) {
        session->ttsProcessor = std::make_shared<va::pipeline::TTSFrameProcessor>(synthesisProvider);
        auto transportWeak = std::weak_ptr<va::transport::LocalTransport>(session->transport);
        session->ttsProcessor->setPlaybackFlush([transportWeak]() {
            if (auto t = transportWeak.lock()) t->flushOutputAudio();
        });
    } else {
        spdlog::warn("[VASDK] No TTS provider available (requested=\"{}\") - running without speech output",
                     ttsParams.providerId);
    }

#ifdef VASDK_HAS_VIZANIM
    // MIC: pipeline-based, real-time AudioFrames → AudioFeaturesFrame → JNI.
    // Must be inserted before transcription so AudioFrames are guaranteed to reach it -
    // LLM and TTS processors only handle TEXT/SYNTHESIS frames and may not forward AUDIO.
    session->micAnimProcessor = std::make_shared<va::thirdparty::AudioInputVizAnimFrameProcessor>(
            audioRingBuffer, audioConfig, audioConfig.sampleRate * 10);

    session->audioFeaturesProcessor = std::make_shared<AudioFeatureSinkProcessor>();
    session->audioFeaturesProcessor->setCallbackTarget(gJvm, session->callbackRef, onAudioFeatures);

    // SYNTH: hardware-callback-driven - features extracted when bytes actually hit the speaker
    session->synthAudioObserver = std::make_shared<SynthAudioObserver>();
    if (!session->synthAudioObserver->initialize(outputAudioConfig, gJvm, session->callbackRef,
                                                 onAudioFeatures)) {
        spdlog::warn(
                "[VASDK] SynthAudioObserver failed to initialize VizAnimProvider - SYNTH animation disabled");
    }
    session->audioSink->setPlaybackCallback(
            [obs = std::weak_ptr<SynthAudioObserver>(session->synthAudioObserver)](const char *data,
                                                                                   size_t bytes) {
                if (auto o = obs.lock()) o->onPlayback(data, bytes);
            });
    spdlog::info("[VASDK] MIC VizAnim processor and synth audio observer configured");
#endif

    //  Tool calling
    // Build a synchronous JNI bridge so C++ tool handlers can call Android APIs.
    // Captures the weak ref and method ID by value; safe for the session lifetime.
    AndroidToolBridge androidToolBridge = [ref = session->callbackRef, onToolCall](
            const std::string &toolName, const std::string &argsJson) -> std::string {
        JNIEnv *tenv = nullptr;
        bool didAttach = false;
        const jint rc = gJvm->GetEnv(reinterpret_cast<void **>(&tenv), JNI_VERSION_1_6);
        if (rc == JNI_EDETACHED) {
            JavaVMAttachArgs targs{JNI_VERSION_1_6, "vasdk-tool", nullptr};
            if (gJvm->AttachCurrentThread(&tenv, &targs) != JNI_OK || !tenv)
                return R"({"error":"failed to attach JVM thread"})";
            didAttach = true;
        } else if (rc != JNI_OK || !tenv) {
            return R"({"error":"invalid JVM state"})";
        }

        std::string result = R"({"error":"callback unavailable"})";
        jobject cb = tenv->NewLocalRef(ref);
        if (cb) {
            jstring jName = tenv->NewStringUTF(toolName.c_str());
            jstring jArgs = tenv->NewStringUTF(argsJson.c_str());
            auto jRes = static_cast<jstring>(tenv->CallObjectMethod(cb, onToolCall, jName, jArgs));
            tenv->DeleteLocalRef(jName);
            tenv->DeleteLocalRef(jArgs);
            tenv->DeleteLocalRef(cb);
            if (jRes) {
                const char *cstr = tenv->GetStringUTFChars(jRes, nullptr);
                result = cstr;
                tenv->ReleaseStringUTFChars(jRes, cstr);
                tenv->DeleteLocalRef(jRes);
            }
        }
        if (didAttach) gJvm->DetachCurrentThread();
        return result;
    };

    session->toolDispatcher = std::make_shared<va::llm::ToolDispatchProcessor>();
    // HINT: the more tools you add the less likely they'll be called, it is a limitation of
    // the embedded models.
    registerCommunicationTools(session.get(), androidToolBridge);
    //registerSystemTools(session.get(), androidToolBridge);
    //registerMediaTools(session.get(), androidToolBridge);
    registerNotificationAndAlarmTools(session.get(), androidToolBridge);
    registerCalendarTools(session.get(), androidToolBridge);
    //registerLocationTools(session.get(), androidToolBridge);
    //registerWebTools(session.get(), androidToolBridge);
    registerCameraTools(session.get(), androidToolBridge);
    //registerAndroidTools(session.get(), androidToolBridge);

    const bool hasTools = !session->toolDispatcher->getToolNames().empty();
    const std::string effectiveFormatInstruction =
            hasTools ? formatInstruction + toolCallExceptionClauseStr : formatInstruction;

    if (useLocalLLM) {
        std::string toolDescs = session->toolDispatcher->renderToolDescriptions(
                llmTransport->getPromptChatFormat());
        va::llm::LLMConfig updated = llmTransport->getConfig();
        updated.systemPrompt = toolDescs.empty() ? systemPromptStr : (systemPromptStr + "\n\n" +
                                                                      toolDescs);
        llmTransport->setConfig(updated);
        // Formats on the base {"text" | "tools"} JSON protocol (empty toolCallTags:
        // gemma, chatml, llama3, ...) get their entire response contract from the
        // tool prompt embedded above - appending the response format instruction after it
        // contradicts that contract (its examples are all text-shaped and it bans
        // keys outside its schema, i.e. "tools"), so small models never emitted a
        // tool call.
        //
        // Marker-token formats (LFM2/LFM2.5) have the same problem from the other
        // direction: their tool prompt teaches the native <|tool_call_start|> syntax,
        // but a trailing response format instruction block ("always reply with valid
        // JSON") is text-shaped and dominant for small models - they output a JSON
        // text reply instead of the native tokens. renderToolPrompt() for these
        // formats embeds the text-response schema inline, so no separate block is
        // needed.
        const auto activeChatFmt = llmTransport->getPromptChatFormat();
        const bool toolPromptOwnsContract =
                !toolDescs.empty() &&
                (va::llm::PromptTemplate::create(activeChatFmt)->toolCallTags().first.empty() ||
                 activeChatFmt == va::llm::ChatFormat::LFM2_5 ||
                 activeChatFmt == va::llm::ChatFormat::LFM2);
        llmTransport->setResponseFormatInstruction(
                toolPromptOwnsContract ? std::string{} : effectiveFormatInstruction);
    } else {
        llmTransport->setResponseFormatInstruction(effectiveFormatInstruction);
        auto sysTemplate = va::llm::PromptTemplate::create(llmTransport->getPromptChatFormat());
        sysTemplate->addSystem(systemPromptStr + "\n\n{tool_descriptions}");
        llmTransport->setPromptTemplate(std::move(sysTemplate));
        llmTransport->registerTemplateVar("tool_descriptions",
                                          [td = session->toolDispatcher](va::llm::ChatFormat fmt) {
                                              return td->renderToolDescriptions(fmt);
                                          });
    }

    session->llmProcessor->setToolDispatcher(session->toolDispatcher);
    spdlog::info("[VASDK] Tool dispatcher configured with {} tools",
                 session->toolDispatcher->getToolNames().size());

    // Bridge processor wires native events back to the Java layer
    session->bridge = std::make_shared<VasdkBridgeProcessor>();
    session->bridge->setCallbackTarget(gJvm, session->callbackRef, onTranscript, onStatus,
                                       onLlmResponse, onError);

    // prepare the agent state processor
    session->agentStateProcessor = std::make_shared<va::pipeline::AgentStateProcessor>();

    session->agentStateProcessor->setAlwaysOn(true);

    session->triggerProcessor = std::make_shared<va::pipeline::TriggerProcessor>();

    session->bridge->setAudioRingBuffer(audioRingBuffer);

    auto transportInput = std::make_shared<va::pipeline::TransportInputProcessor>();
    transportInput->addTransport(session->transport);
    auto transportOutput = std::make_shared<va::pipeline::TransportOutputProcessor>();
    transportOutput->addTransport(session->transport, va::transport::FrameType::SYNTHESIS);

    session->pipeline = std::make_shared<va::pipeline::Pipeline>();
    // MIC viz anim sits early (before transcription) so AudioFrames reach it before LLM/TTS
    // absorb or queue them. Matches reference pipeline order in main_pipeline.cpp.
    auto &chain = session->pipeline->addProcessor(transportInput).addProcessor(
            session->triggerProcessor).addProcessor(session->vadFrameProcessor).addProcessor(
            session->turnDetectionFrameProcessor);
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    // Before transcription so the identification precedes the utterance's TRANSCRIPTION frame.
    if (session->speakerIdProcessor) chain.addProcessor(session->speakerIdProcessor);
#endif
    chain.addProcessor(session->agentStateProcessor);

#ifdef VASDK_HAS_VIZANIM
    if (session->micAnimProcessor) {
        chain.addProcessor(session->micAnimProcessor).addProcessor(session->audioFeaturesProcessor);
    }
#endif

    chain.addProcessor(session->transcriptionProcessor).addProcessor(
            session->llmProcessor).addProcessor(
            session->toolDispatcher)  // intercept TOOL_CALL frames, execute tools
            .addProcessor(session->bridge);          // intercept TEXT frames from LLM before TTS

    // ttsProcessor is only set when a TTS provider was actually constructed above (runtime
    // choice + compile-time availability), so no #if guard is needed here.
    if (session->ttsProcessor) {
        chain.addProcessor(session->ttsProcessor);
    }

    chain.addProcessor(transportOutput);

    if (!session->pipeline->build()) {
        spdlog::error("[VASDK] Failed to build pipeline");
        env->DeleteWeakGlobalRef(session->callbackRef);
        return JNI_FALSE;
    }

    gSession = std::move(session);
    spdlog::info("[VASDK] Session initialized");
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
    if (!gSession->transport->start()) {
        spdlog::error("[VASDK] Failed to start transport");
        return JNI_FALSE;
    }
    if (!gSession->pipeline->start()) {
        spdlog::error("[VASDK] Failed to start pipeline");
        gSession->transport->stop();
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
    gSession->pipeline->stop();
    gSession->transport->stop();
    spdlog::info("[VASDK] Pipeline stopped");
}

//  nativeRelease

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeRelease(JNIEnv *env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;
    gSession->pipeline->stop();
    gSession->transport->stop();
    if (gSession->callbackRef) {
        env->DeleteWeakGlobalRef(gSession->callbackRef);
        gSession->callbackRef = nullptr;
    }
    gSession.reset();
    spdlog::info("[VASDK] Session released");
}

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeTrigger(JNIEnv * /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession || !gSession->triggerProcessor) return;
    // Used by the UI's cancel-speech button - a BUTTON_PRESS while SPEAKING
    // interrupts the active TTS the same way a user barging in by voice would.
    // TriggerProcessor::trigger() is safe to call from this JNI/UI thread - it
    // queues the frame and lets it flow downstream through the pipeline like
    // any other frame, rather than reaching into another processor directly.
    gSession->triggerProcessor->trigger(va::transport::TriggerType::BUTTON_PRESS);
}

//  nativeResetConversation

extern "C" JNIEXPORT void JNICALL
Java_com_vasdk_android_VasdkEngine_nativeResetConversation(JNIEnv * /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession || !gSession->llmProcessor) return;
    // Interrupt any in-flight TTS/response the same way the cancel button does, then
    // drop the LLM's message history so the next utterance starts a fresh conversation
    // without tearing down and re-initializing the whole pipeline.
    if (gSession->triggerProcessor) {
        gSession->triggerProcessor->trigger(va::transport::TriggerType::BUTTON_PRESS);
    }
    gSession->llmProcessor->clearHistory();
    spdlog::info("[VASDK] Conversation history cleared");
}

//  nativeGetPromptPreview

extern "C" JNIEXPORT jstring JNICALL
Java_com_vasdk_android_VasdkEngine_nativeGetPromptPreview(JNIEnv *env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession || !gSession->llmTransport) return env->NewStringUTF("");

    std::string rawPrompt;
    if (gSession->useLocalLLM) {
        // LlamaCppTransport renders the actual request via the GGUF's own embedded chat
        // template, not this PromptTemplate hierarchy - reconstruct an equivalent preview
        // from the same system prompt (tool descriptions are already baked into it, see
        // the local branch of nativeInit above).
        auto tmpl = va::llm::PromptTemplate::create(gSession->llmTransport->getPromptChatFormat());
        tmpl->addSystem(gSession->llmTransport->getConfig().systemPrompt);
        rawPrompt = tmpl->render();
    } else {
        rawPrompt = gSession->llmTransport->previewRequestPayload();
        try {
            rawPrompt = nlohmann::json::parse(rawPrompt).dump(2);
        } catch (const std::exception &e) {
            spdlog::warn("[VASDK] Failed to pretty-print prompt preview: {}", e.what());
        }
    }

    nlohmann::json info;
    info["isLocal"] = gSession->useLocalLLM;
    info["systemPrompt"] = gSession->baseSystemPrompt;
    info["tools"] = gSession->toolDispatcher ? gSession->toolDispatcher->getEnabledToolsJson()
                                              : nlohmann::json::array();
    info["rawPrompt"] = rawPrompt;
    return env->NewStringUTF(info.dump().c_str());
}
