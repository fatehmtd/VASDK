#pragma once

#include "VizAnimSupport.h"
#include "audio_sink/AAudioSink.h"
#include "audio_source/AAudioSource.h"

#include <VASDK/engine/Runtime.h>
#include <VASDK/engine/Session.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
#include <VASDK/SpeakerIdentificationProcessor.h>
#endif

#include <jni.h>
#include <memory>
#include <string>

// The app's one conversation: a VAEngine runtime and session, with what the Android app adds to it.
struct VasdkSession {
    std::shared_ptr<va::engine::Runtime> runtime;
    std::unique_ptr<va::engine::Session> engine;
    std::shared_ptr<va::android::AAudioSource> audioSource;
    std::shared_ptr<va::android::AAudioSink> audioSink;
    std::shared_ptr<va::llm::LLMTransport> llmTransport;   // the session's: local (llama.cpp) or the config's endpoint
    std::shared_ptr<va::llm::ToolDispatchProcessor> toolDispatcher;   // the session's, for ToolRegistration
    bool useLocalLLM{false};
    // The base instruction text before tool descriptions are appended (see nativeInit) -
    // kept separately so the "model info" screen can show it apart from the tools list.
    std::string baseSystemPrompt;
#ifdef VASDK_HAS_PYANNOTE_SPEAKER
    std::shared_ptr<va::pipeline::SpeakerIdentificationProcessor> speakerIdProcessor;
#endif
#ifdef VASDK_HAS_VIZANIM
    std::shared_ptr<va::thirdparty::audio::AudioInputVizAnimFrameProcessor> micAnimProcessor;
    std::shared_ptr<AudioFeatureSinkProcessor> audioFeaturesProcessor;
    std::shared_ptr<SynthAudioObserver> synthAudioObserver;
#endif
    jweak callbackRef{nullptr};
};
