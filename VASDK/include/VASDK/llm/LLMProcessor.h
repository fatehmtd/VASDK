#pragma once

#include <VASDK/FrameProcessor.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/LLMFirePolicy.h>
#include <VASDK/llm/StreamingReplyExtractor.h>
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
#include <deque>
#include <functional>

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

    /**
     * Speak streamed replies sentence by sentence: while the transport streams deltas, each
     * complete sentence is pushed downstream as a TextFrame{isFinal=false}; the reply then ends
     * with one isFinal=true frame carrying only the not-yet-spoken text plus isExpecting/choices.
     * Works with any transport that streams (and is a no-op for non-streaming ones).
     */
    void setStreamSpeech(bool enabled) { _streamSpeech = enabled; }

    /**
     * Phrases spoken (in rotation) the moment a streamed reply turns out to be a tool call, so the
     * caller hears "One moment." instead of silence during the tool round trip. Needs
     * setStreamSpeech(true). Empty (the default) disables it.
     */
    void setToolFillerPhrases(std::vector<std::string> phrases) { _toolFillerPhrases = std::move(phrases); }

    /**
     * Start the LLM before the turn is confirmed: when the VAD reports a sustained pause (ENDING)
     * while the agent is listening, the request fires on the cached partial transcript (at least
     * minWords words). Its reply is held back - text frames are not pushed and tool calls are
     * never dispatched - until the turn ends with a final transcript matching that partial; then
     * the held sentences are released at once. If the user resumes, or the final transcript
     * differs, the request is cancelled and rolled back from history. Needs streaming STT partials.
     */
    void setSpeculativeFiring(bool enabled, int minWords = 3) {
        _speculativeFiring = enabled;
        _speculativeMinWords = minWords;
    }

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
    // Fires a request for a confirmed user turn; confirms or replaces a pending speculation first.
    void dispatchLLMRequest(const std::string& text, float confidence);
    void dispatchRequestNow(const std::string& text, float confidence);

    void handleVadStateFrame(std::shared_ptr<transport::VADStateFrame> frame);
    void cancelSpeculation(const char* reason);
    bool _speculativeFiring{false};
    int  _speculativeMinWords{3};
    std::atomic<transport::AgentState> _agentState{transport::AgentState::IDLE};
    struct Speculation {
        bool active = false, confirmed = false, discarded = false;
        std::string text;
        std::vector<std::shared_ptr<transport::TextFrame>> held;  // speech waiting for confirmation
    };
    std::mutex  _specMutex;
    Speculation _spec;

    // LLM requests run on their own thread, one at a time, so interrupts are handled while a
    // reply is still generating. A job queued in an older generation (barge-in happened) is
    // skipped without running.
    void enqueueRequest(std::function<void()> job);
    void requestWorkerLoop();
    std::thread             _requestThread;
    std::mutex              _requestMutex;
    std::condition_variable _requestCv;
    std::deque<std::pair<uint64_t, std::function<void()>>> _requestQueue;
    bool                    _requestStop{false};

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
    std::mutex _streamingMutex;  // guards _streamingBuffer and _extractor (request thread vs. interrupts)
    std::vector<std::shared_ptr<transport::TextFrame>> _streamingBuffer;
    bool _streamSpeech{false};
    std::unique_ptr<StreamingReplyExtractor> _extractor;  // current streamed reply
    std::atomic<bool> _speechOpen{false};  // sentence frames went out for a reply that has no final frame yet
    std::vector<std::string> _toolFillerPhrases;
    size_t _nextFiller{0};
    bool _fillerSent{false};  // for the current streamed reply; guarded by _streamingMutex
    // A tool batch was dispatched and its follow-up reply hasn't started: keep the spoken turn open.
    std::atomic<bool> _awaitingToolFollowUp{false};
    void pushSpeech(const std::string& text, bool isFinal, bool isExpecting = false,
                    std::vector<transport::TextFrame::Choice> choices = {});
    std::atomic<int> _toolCallDepth{0};
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
    // Generation of the request job running on the worker; its streamed frames are stale once
    // _currentGeneration moves on (a later dispatch must not revive them).
    std::atomic<uint64_t> _runningGeneration{0};
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
