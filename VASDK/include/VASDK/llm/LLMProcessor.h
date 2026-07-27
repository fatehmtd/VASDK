#pragma once

#include <VASDK/FrameProcessor.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/LLMFirePolicy.h>
#include <VASDK/metrics/MetricsCollector.h>
#include <VASDK/metrics/MetricHandles.h>
#include <VASDK/metrics/MetricsConfig.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <atomic>

namespace va {
namespace llm {

// Forward declaration - include ToolDispatchProcessor.h in .cpp only
class ToolDispatchProcessor;

class VASDK_EXPORT LLMProcessor : public pipeline::FrameProcessor {
public:
    explicit LLMProcessor(std::shared_ptr<LLMTransport> transport);
    virtual ~LLMProcessor();

    std::string getName() const override { return "LLMProcessor"; }
    bool initialize() override;
    bool start() override;
    void stop() override;
    void release() override;
    void processFrame(std::shared_ptr<transport::Frame> frame,
                      pipeline::FlowDirection direction) override;

    void setConfig(const LLMConfig& config);
    const LLMConfig& getConfig() const;

    void setHistoryEnabled(bool enabled);
    void clearHistory();
    void setMaxHistoryLength(size_t maxMessages);
    const std::vector<ConversationMessage>& getHistory() const;

    void setProcessPartialTranscriptions(bool process) {
        _processPartialTranscriptions = process;
    }

    void setConfidenceThreshold(float threshold) {
        _confidenceThreshold = threshold;
    }

    void setMetricsConfig(const va::metrics::LLMMetricsConfig& config);
    const va::metrics::LLMMetricsConfig& getLLMMetricsConfig() const { return _metrics.config; }

    void setFirePolicy(std::shared_ptr<LLMFirePolicyInterface> policy) {
        if (policy) _firePolicy = policy;
    }

    const LLMFirePolicyInterface& firePolicy() const { return *_firePolicy; }

    void setFinalWatchdogTimeoutMs(uint32_t ms) { _finalWatchdogTimeoutMs = ms; }

    void setToolDispatcher(std::shared_ptr<ToolDispatchProcessor> dispatcher);

    void setReasoningTags(std::string openTag, std::string closeTag) {
        _reasoningTagOpen = std::move(openTag);
        _reasoningTagClose = std::move(closeTag);
        _reasoningTagsOverridden = true;
    }

protected:
    bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
    void processDownstream(std::shared_ptr<transport::Frame> frame);
    void onTransportFrameReceived(std::shared_ptr<transport::Frame> frame);

    // Dispatches on agentFrame->transition. Returns true if it already pushed
    // agentFrame downstream and dispatched an LLM request - processDownstream
    // must not push the frame again in that case.
    bool handleAgentStateTransition(std::shared_ptr<transport::AgentStateFrame> agentFrame);
    void handleInterruptTransition(transport::AgentStateTransition t);
    void handleUserStartedSpeakingTransition();
    void handleTranscriptionReadyTransition();
    bool handleUserStoppedSpeakingTransition(std::shared_ptr<transport::AgentStateFrame> agentFrame);

private:
    std::shared_ptr<LLMTransport> _transport;
    bool _processPartialTranscriptions = false;
    float _confidenceThreshold = 0.5f;
    bool _partialAlreadySent = false;
    std::shared_ptr<LLMFirePolicyInterface> _firePolicy{ std::make_shared<EagerFirePolicy>() };

    mutable std::mutex _partialCacheMutex;
    std::string        _latestPartialText;
    float              _latestPartialConfidence{ 0.0f };

    std::mutex  _pendingTurnMutex;
    bool        _pendingTurnEnd{false};
    std::string _pendingFallbackText;
    float       _pendingFallbackConf{0.0f};
    std::string _accumulatedFinalText;
    float       _accumulatedFinalConf{0.0f};

    uint32_t                _finalWatchdogTimeoutMs{2000};
    std::thread             _finalWatchdogThread;
    std::mutex              _watchdogMutex;
    std::condition_variable _watchdogCv;
    bool                    _watchdogArmed{false};
    bool                    _watchdogStop{false};

    void startFinalWatchdog();
    void cancelFinalWatchdog();
    void finalWatchdogLoop();
    void dispatchLLMRequest(const std::string& text, float confidence);

    struct ToolCall {
        std::string toolName;
        std::string argsJson;
    };

    struct StructuredResponse {
        std::string text;
        std::vector<ToolCall> tools;
        bool isExpecting = false;
        std::vector<transport::TextFrame::Choice> choices;
        bool hasTool() const { return !tools.empty(); }
    };
    static StructuredResponse parseStructuredResponse(const std::string& raw,
                                                       const std::string& reasoningTagOpen,
                                                       const std::string& reasoningTagClose);

    // Resolves the (open, close) reasoning tags to use for the next parse: the explicit
    // override from setReasoningTags() if one was set, otherwise the active ChatFormat's
    // PromptTemplate::reasoningTags() (re-derived each call since the transport's
    // format/template can change mid-session, e.g. GoalManager intent switches).
    std::pair<std::string, std::string> effectiveReasoningTags() const;

    std::string _reasoningTagOpen;
    std::string _reasoningTagClose;
    bool _reasoningTagsOverridden{false};

    std::shared_ptr<ToolDispatchProcessor> _toolDispatcher;
    std::vector<std::shared_ptr<transport::TextFrame>> _streamingBuffer;
    int _toolCallDepth{0};
    static constexpr int kMaxToolCallDepth = 5;

    std::atomic<int> _pendingToolCount{0};
    std::vector<std::pair<std::string,std::string>> _collectedToolResults;
    std::mutex _toolResultsMutex;

    // Last dispatched tool batch as sorted (name, argsJson) pairs, plus the
    // speech text of the response that requested it. Used to break
    // self-imitation loops (the model re-requesting the exact batch whose
    // results it just received) and to speak the last suppressed text instead
    // of going silent when the depth cap trips. Guarded by _toolResultsMutex.
    std::vector<std::pair<std::string,std::string>> _lastToolBatch;
    std::string _lastToolBatchText;

    std::atomic<uint64_t> _currentGeneration{0};
    std::atomic<uint64_t> _dispatchGeneration{0};
    uint64_t _toolBatchGeneration{0};

    struct Metrics {
        va::metrics::LLMMetricsConfig config;
        va::metrics::SamplerHandle    ttft;
        va::metrics::CounterHandle    tokens;
        va::metrics::CounterHandle    requests;
        va::metrics::GaugeHandle      ttftAlert;
    } _metrics;
    std::chrono::steady_clock::time_point _llmRequestStart;
    bool _firstTokenReceived{false};
};

} // namespace llm
} // namespace va
