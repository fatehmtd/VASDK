#pragma once

#include <VASDK/HttpTransport.h>
#include <VASDK/llm/LLMTypes.h>
#include <VASDK/llm/PromptTemplate.h>
#include <VASDK/metrics/MetricHandles.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>

namespace va {
namespace llm {

class VASDK_EXPORT LLMTransport : public transport::HttpTransport {
public:
    LLMTransport(LLMProvider provider, const LLMConfig& config,
                 std::shared_ptr<va::network::HttpInterface> client = nullptr);

    virtual ~LLMTransport();

    bool initialize() override;
    void release() override;

    bool sendFrame(std::shared_ptr<transport::Frame> frame) override;

    std::string getName() const override { return "LLMTransport"; }
    std::string getDescription() const override {
        return "Transport for Large Language Model services (OpenAI, Anthropic, etc.)";
    }

    virtual void cancel() {}

    void setConfig(const LLMConfig& config);
    const LLMConfig& getConfig() const { return _config; }

    virtual void addMessageToHistory(MessageRole role, const std::string& content);
    virtual void clearHistory();

    const std::vector<ConversationMessage>& getHistory() const { return _conversationHistory; }

    void setMaxHistoryLength(size_t maxMessages) { _maxHistoryLength = maxMessages; }
    void setHistoryEnabled(bool enabled) { _historyEnabled = enabled; }

    void setPromptTemplate(std::unique_ptr<PromptTemplate> promptTemplate);
    void clearPromptTemplate();
    bool hasPromptTemplate() const { return _promptTemplate != nullptr; }

    // Chat format used to render the active PromptTemplate into the request payload.
    // Defaults to the idiomatic format for the transport's provider; override for
    // OpenAI-compatible (or otherwise provider-adjacent) backends whose server-side
    // template expects a different message encoding.
    void setPromptChatFormat(ChatFormat format) { _llmPromptChatFormat = format; }
    ChatFormat getPromptChatFormat() const { return _llmPromptChatFormat; }

    // Set once at startup; automatically appended as a system message on every
    // setPromptTemplate call (covers IntentRouter / GoalManager template swaps).
    // For config-based transports (e.g. LlamaCpp) that bypass setPromptTemplate,
    // also appends directly to the first SYSTEM entry in conversation history.
    void setResponseFormatInstruction(std::string instruction);

    using TemplateVarResolver = std::function<std::string(ChatFormat)>;
    void registerTemplateVar(const std::string& varName, TemplateVarResolver resolver);
    void unregisterTemplateVar(const std::string& varName);

    void injectToolResult(const std::string& toolName, const std::string& resultJson);
    void injectToolResults(const std::vector<std::pair<std::string,std::string>>& results);

    // Renders the request payload this transport would send right now, given its current
    // config/history, without performing the request. For UI/debug surfaces (e.g. a
    // "model info" screen) that want to show what's actually being sent.
    std::string previewRequestPayload() { return buildRequestPayload(""); }

protected:
    bool processIncomingFrame(std::shared_ptr<transport::Frame> frame);
    std::string extractUserMessage(std::shared_ptr<transport::Frame> frame) const;
    void applyProviderAuthHeaders(std::map<std::string, std::string>& headers) const;

    virtual std::string sendLLMRequest(const std::string& userMessage);
    virtual std::string buildRequestPayload(const std::string& userMessage);
    virtual std::string parseResponse(const std::string& response);
    virtual bool parseStreamingData(const std::string& data,
                                    std::string& deltaText,
                                    bool& isDone);

    // Renders the active PromptTemplate in its own format; returns "" when no template is set.
    // Acquires _historyMutex internally - do NOT call while holding it.
    std::string renderTemplate();

    static const char* roleToString(MessageRole role);

    LLMProvider _provider;
    LLMConfig _config;
    std::vector<ConversationMessage> _conversationHistory;
    size_t _maxHistoryLength = 20;
    bool _historyEnabled = true;
    std::atomic<uint64_t> _nextSequenceId{1};

    std::mutex _historyMutex;
    std::unique_ptr<PromptTemplate> _promptTemplate;
    ChatFormat _llmPromptChatFormat;
    std::string _responseFormatInstruction;
    std::unordered_map<std::string, TemplateVarResolver> _templateVarResolvers;

    va::metrics::SamplerHandle _apiRoundtripSampler;

    std::atomic<bool> _initialized{false};

    // True while injectToolResults() runs the follow-up request. Local
    // transports use it to constrain generation (e.g. ban the tool-call start
    // token) so the model produces a final answer instead of re-emitting the
    // tool call it just made.
    std::atomic<bool> _forcingFinalAnswer{false};
};

} // namespace llm
} // namespace va
