#pragma once

// MetricsBinder.h must be included only from .cpp files (application layer).
// It pulls in all pipeline processor headers; including it from another header
// would create circular dependencies.

#include "MetricsConfig.h"
#include "UtteranceObserver.h"
#include <VASDK/Pipeline.h>
#include <VASDK/VADFrameProcessor.h>
#include <VASDK/TurnDetectionProcessor.h>
#include <VASDK/TranscriptionProcessor.h>
#include <VASDK/TTSFrameProcessor.h>
#include <VASDK/AgentStateProcessor.h>
#include <VASDK/llm/LLMProcessor.h>
#include <memory>
#include <string>

namespace va {
namespace metrics {

/**
 * Wires MetricsConfig sub-configs and the UtteranceObserver to processors in a
 * Pipeline, keyed by getName().
 *
 * Replaces the per-processor setMetricsConfig() block in main_pipeline.cpp with
 * a single call:
 *
 *   MetricsBinder::bind(*pipeline, metricsCfg, metricsReporter);
 *
 * Adding a new processor with metrics only requires adding one branch here.
 */
class MetricsBinder {
public:
    MetricsBinder() = delete;

    static void bind(
        const pipeline::Pipeline&                   pipeline,
        const MetricsConfig&                        cfg,
        std::shared_ptr<UtteranceObserverInterface> utteranceObserver = nullptr)
    {
        for (const auto& proc : pipeline.getProcessors()) {
            if (!proc) continue;
            const std::string name = proc->getName();

            if (name == "VADFrameProcessor") {
                if (auto* p = dynamic_cast<pipeline::VADFrameProcessor*>(proc.get()))
                    p->setMetricsConfig(cfg.vad);
            }
            else if (name == "TurnDetectionProcessor") {
                if (auto* p = dynamic_cast<pipeline::TurnDetectionProcessor*>(proc.get()))
                    p->setMetricsConfig(cfg.turnDetection);
            }
            else if (name == "TranscriptionFrameProcessor") {
                if (auto* p = dynamic_cast<pipeline::TranscriptionFrameProcessor*>(proc.get()))
                    p->setMetricsConfig(cfg.transcription);
            }
            else if (name == "TTSFrameProcessor") {
                if (auto* p = dynamic_cast<pipeline::TTSFrameProcessor*>(proc.get()))
                    p->setMetricsConfig(cfg.tts);
            }
            else if (name == "AgentStateProcessor") {
                if (auto* p = dynamic_cast<pipeline::AgentStateProcessor*>(proc.get())) {
                    p->setMetricsConfig(cfg.defaultProcessor);
                    if (utteranceObserver)
                        p->setUtteranceObserver(utteranceObserver);
                }
            }
            else if (name == "LLMProcessor") {
                if (auto* p = dynamic_cast<llm::LLMProcessor*>(proc.get()))
                    p->setMetricsConfig(cfg.llm);
            }
        }
    }
};

} // namespace metrics
} // namespace va
