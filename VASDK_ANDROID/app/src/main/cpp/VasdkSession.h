#pragma once

#include "VizAnimSupport.h"
#include "audio_sink/AAudioSink.h"
#include "audio_source/AAudioSource.h"

#include <VASDK/AgentStateProcessor.h>
#include <VASDK/LocalTransport.h>
#include <VASDK/Pipeline.h>
#include <VASDK/TTSFrameProcessor.h>
#include <VASDK/TranscriptionProcessor.h>
#include <VASDK/TriggerProcessor.h>
#include <VASDK/TurnDetectionProcessor.h>
#include <VASDK/VADFrameProcessor.h>
#include <VASDK/llm/LLMProcessor.h>
#include <VASDK/llm/ToolDispatchProcessor.h>

#include <jni.h>
#include <memory>
#include <string>

class VasdkBridgeProcessor;

struct VasdkSession {
    std::shared_ptr<va::android::AAudioSource> audioSource;
    std::shared_ptr<va::android::AAudioSink> audioSink;
    std::shared_ptr<va::transport::LocalTransport> transport;
    std::shared_ptr<VasdkBridgeProcessor> bridge;
    std::shared_ptr<va::pipeline::Pipeline> pipeline;
    std::shared_ptr<va::pipeline::TriggerProcessor> triggerProcessor;
    std::shared_ptr<va::pipeline::VADFrameProcessor> vadFrameProcessor;
    std::shared_ptr<va::pipeline::TurnDetectionProcessor> turnDetectionFrameProcessor;
    std::shared_ptr<va::pipeline::AgentStateProcessor> agentStateProcessor;
    std::shared_ptr<va::pipeline::TranscriptionFrameProcessor> transcriptionProcessor;
    std::shared_ptr<va::llm::LLMProcessor> llmProcessor;
    std::shared_ptr<va::llm::LLMTransport> llmTransport;
    bool useLocalLLM{false};
    // The base instruction text before tool descriptions are appended (see nativeInit) -
    // kept separately so the "model info" screen can show it apart from the tools list.
    std::string baseSystemPrompt;
    std::shared_ptr<va::llm::ToolDispatchProcessor> toolDispatcher;
    std::shared_ptr<va::pipeline::TTSFrameProcessor> ttsProcessor;
#ifdef VASDK_HAS_VIZANIM
    std::shared_ptr<va::thirdparty::AudioInputVizAnimFrameProcessor> micAnimProcessor;
    std::shared_ptr<AudioFeatureSinkProcessor> audioFeaturesProcessor;
    std::shared_ptr<SynthAudioObserver> synthAudioObserver;
#endif
    jweak callbackRef{nullptr};
};
