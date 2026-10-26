#include <VASDK/llm/LLMTransport.h>
#include <VASDK/profiling/CTrack.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <cctype>
#include <algorithm>
#include <spdlog/spdlog.h>

#ifdef ERROR
#undef ERROR
#endif

using json = nlohmann::json;

namespace {

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(begin, end - begin);
}

bool hasHttpScheme(const std::string& endpoint) {
    return endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0;
}

bool containsWhitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

bool normalizeAndValidateEndpoint(va::llm::LLMConfig& config, std::string& errorMessage) {
    const std::string trimmed = trimCopy(config.endpoint);
    if (trimmed.empty()) {
        errorMessage = "API endpoint not provided";
        return false;
    }

    if (trimmed != config.endpoint) {
        spdlog::warn("[LLMTransport] Endpoint had surrounding whitespace; trimming before use");
        config.endpoint = trimmed;
    }

    if (!hasHttpScheme(config.endpoint)) {
        errorMessage = "API endpoint must start with http:// or https:// (received: '" + config.endpoint + "')";
        return false;
    }

    if (containsWhitespace(config.endpoint)) {
        errorMessage = "API endpoint contains whitespace and is invalid (received: '" + config.endpoint + "')";
        return false;
    }

    return true;
}

} // namespace

namespace va {
namespace llm {

LLMTransport::LLMTransport(LLMProvider provider, const LLMConfig& config,
                           std::shared_ptr<va::network::HttpInterface> client)
    : transport::HttpTransport(std::move(client)), _provider(provider), _config(config),
      _llmPromptChatFormat(PromptTemplate::defaultFormatForProvider(provider).value_or(ChatFormat::OPENAI)) {
    if (!_config.systemPrompt.empty()) {
        addMessageToHistory(MessageRole::SYSTEM, _config.systemPrompt);
    }
}

LLMTransport::~LLMTransport() {
    stop();
    release();
}

bool LLMTransport::initialize() {
    if (_initialized) return true;

    if (_config.apiKey.empty()) {
        fireOnError(static_cast<int>(LLMErrorCode::MissingApiKey), "API key not provided");
        return false;
    }
    std::string endpointValidationError;
    if (!normalizeAndValidateEndpoint(_config, endpointValidationError)) {
        fireOnError(static_cast<int>(LLMErrorCode::InvalidEndpoint), endpointValidationError);
        return false;
    }

    if (!transport::HttpTransport::initialize()) return false;

    if (_metricsConfig.enabled && _collector) {
        using namespace va::metrics;
        _apiRoundtripSampler.initialize(_collector,
            MetricDescriptor{"llm_api_roundtrip_ms", MetricType::Sampler,
                             "LLM API HTTP roundtrip latency (ms)",
                             _metricsConfig.apiRoundtripWindow});
    }

    spdlog::info("[LLMTransport] Initialized with provider {}, endpoint: {}",
                 static_cast<int>(_provider), _config.endpoint);
    _initialized = true;
    return true;
}

void LLMTransport::release() {
    transport::HttpTransport::release();
    clearHistory();
    _initialized = false;
}

bool LLMTransport::sendFrame(std::shared_ptr<transport::Frame> frame) {
    return processIncomingFrame(frame);
}

void LLMTransport::setConfig(const LLMConfig& config) {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _config = config;
    if (!_conversationHistory.empty() &&
        _conversationHistory[0].role == MessageRole::SYSTEM) {
        _conversationHistory[0].content = _config.systemPrompt;
    }
}

void LLMTransport::addMessageToHistory(MessageRole role, const std::string& content) {
    if (!_historyEnabled) return;

    std::lock_guard<std::mutex> lock(_historyMutex);
    ConversationMessage msg;
    msg.role = role;
    msg.content = content;
    msg.timestamp = transport::Frame::getCurrentTimestamp();
    _conversationHistory.push_back(msg);

    while (_conversationHistory.size() > _maxHistoryLength) {
        for (auto it = _conversationHistory.begin(); it != _conversationHistory.end(); ++it) {
            if (it->role != MessageRole::SYSTEM) {
                _conversationHistory.erase(it);
                break;
            }
        }
    }
}

void LLMTransport::clearHistory() {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _conversationHistory.clear();
    if (!_config.systemPrompt.empty()) {
        ConversationMessage msg;
        msg.role = MessageRole::SYSTEM;
        msg.content = _config.systemPrompt;
        msg.timestamp = transport::Frame::getCurrentTimestamp();
        _conversationHistory.push_back(msg);
    }
}

void LLMTransport::injectToolResults(
    const std::vector<std::pair<std::string,std::string>>& results)
{
    const bool useNativeToolRole = (_provider == LLMProvider::LLAMA_CPP ||
                                    _provider == LLMProvider::LFM);

    if (useNativeToolRole) {
        // LFM / llama.cpp: inject each result as a TOOL-role message so the
        // model's embedded Jinja template can wrap it in the correct tokens
        // (e.g. <|tool_response_start|>…<|tool_response_end|> for LFM2).
        // The content must stay pure tool output: the template wraps it in the
        // tool_response block, which the model reads as data to relay to the
        // user, not as instructions - an "answer now" nudge placed here gets
        // echoed back verbatim (and spoken by TTS). Re-emission of the tool
        // call is instead prevented at the token level by the transport via
        // _forcingFinalAnswer.
        for (auto& [toolName, resultJson] : results) {
            addMessageToHistory(MessageRole::TOOL, resultJson);
        }
    } else {
        for (auto& [toolName, resultJson] : results) {
            addMessageToHistory(MessageRole::USER,
                "Tool \"" + toolName + "\" returned: " + resultJson);
        }
        addMessageToHistory(MessageRole::USER,
            "All tools have returned. Reply with your final answer now.");
    }

    _forcingFinalAnswer.store(true, std::memory_order_relaxed);
    std::string response = sendLLMRequest("");
    _forcingFinalAnswer.store(false, std::memory_order_relaxed);
    if (!response.empty()) {
        addMessageToHistory(MessageRole::ASSISTANT, response);

        if (!_config.streaming) {
            auto responseFrame = std::make_shared<transport::TextFrame>();
            responseFrame->text = response;
            responseFrame->isFinal = true;
            responseFrame->timestamp = transport::Frame::getCurrentTimestamp();
            responseFrame->sequenceId = _nextSequenceId++;
            responseFrame->sourceId = getName();
            enqueueReceivedFrame(responseFrame);
        }

        updateSentStats(response.length());
    }
}

void LLMTransport::injectToolResult(const std::string& toolName, const std::string& resultJson) {
    injectToolResults({{ toolName, resultJson }});
}

void LLMTransport::setResponseFormatInstruction(std::string instruction) {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _responseFormatInstruction = std::move(instruction);
    // For config-based transports (e.g. LlamaCpp) that read _conversationHistory directly
    // and never call setPromptTemplate, append to the existing SYSTEM message now.
    if (!_responseFormatInstruction.empty() &&
        !_conversationHistory.empty() &&
        _conversationHistory[0].role == MessageRole::SYSTEM) {
        _conversationHistory[0].content += "\n\n" + _responseFormatInstruction;
    }
}

void LLMTransport::setPromptTemplate(std::unique_ptr<PromptTemplate> tmpl) {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _promptTemplate = std::move(tmpl);
    if (_promptTemplate && !_responseFormatInstruction.empty()) {
        _promptTemplate->addSystem(_responseFormatInstruction);
    }
}

void LLMTransport::clearPromptTemplate() {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _promptTemplate.reset();
}

void LLMTransport::registerTemplateVar(const std::string& varName, TemplateVarResolver resolver) {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _templateVarResolvers[varName] = std::move(resolver);
}

void LLMTransport::unregisterTemplateVar(const std::string& varName) {
    std::lock_guard<std::mutex> lock(_historyMutex);
    _templateVarResolvers.erase(varName);
}

std::string LLMTransport::renderTemplate() {
    std::lock_guard<std::mutex> lock(_historyMutex);
    if (!_promptTemplate) return "";

    // Resolve missing variables directly on the stored template, then unset them again
    // after rendering - dynamically-resolved values (e.g. tool_descriptions, which can
    // change between calls) must not be permanently cached, or the resolver would never
    // be re-invoked on a later render().
    std::vector<std::string> resolvedNow;
    for (const auto& varName : _promptTemplate->missingVariables()) {
        auto it = _templateVarResolvers.find(varName);
        if (it != _templateVarResolvers.end()) {
            _promptTemplate->set(varName, it->second(_promptTemplate->format()));
            resolvedNow.push_back(varName);
        }
    }

    std::string rendered = _promptTemplate->render();

    for (const auto& varName : resolvedNow) {
        _promptTemplate->unset(varName);
    }

    return rendered;
}

const char* LLMTransport::roleToString(MessageRole role) {
    switch (role) {
        case MessageRole::SYSTEM:    return "system";
        case MessageRole::USER:      return "user";
        case MessageRole::ASSISTANT: return "assistant";
        case MessageRole::TOOL:      return "user"; // no dedicated OpenAI/Anthropic role
    }
    return "user";
}

std::string LLMTransport::extractUserMessage(std::shared_ptr<transport::Frame> frame) const {
    switch (frame->getType()) {
    case transport::FrameType::TEXT:
        if (auto textFrame = std::dynamic_pointer_cast<transport::TextFrame>(frame)) {
            return textFrame->text;
        }
        return "";
    case transport::FrameType::TRANSCRIPTION: {
        auto transcriptionFrame = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame);
        if (transcriptionFrame && transcriptionFrame->isFinal) {
            return transcriptionFrame->transcriptionResult.text;
        }
        return "";
    }
    default:
        return "";
    }
}

bool LLMTransport::processIncomingFrame(std::shared_ptr<transport::Frame> frame) {
    if (!frame) return false;

    std::string userMessage = extractUserMessage(frame);

    spdlog::debug("[LLMTransport] Received user message: {}", userMessage);
    if (userMessage.empty()) return false;

    addMessageToHistory(MessageRole::USER, userMessage);

    std::string response = sendLLMRequest(userMessage);
    if (!response.empty()) {
        addMessageToHistory(MessageRole::ASSISTANT, response);

        if (!_config.streaming) {
            auto responseFrame = std::make_shared<transport::TextFrame>();
            responseFrame->text = response;
            responseFrame->timestamp = transport::Frame::getCurrentTimestamp();
            responseFrame->sequenceId = _nextSequenceId++;
            responseFrame->sourceId = getName();
            enqueueReceivedFrame(responseFrame);
        }

        updateSentStats(response.length());
        return true;
    }
    return false;
}

void LLMTransport::applyProviderAuthHeaders(std::map<std::string, std::string>& headers) const {
    switch (_provider) {
    case LLMProvider::OPENAI:
    case LLMProvider::OPENROUTER:
        headers["Authorization"] = "Bearer " + _config.apiKey;
        break;
    case LLMProvider::ANTHROPIC:
        headers["x-api-key"] = _config.apiKey;
        headers["anthropic-version"] = "2023-06-01";
        break;
    default:
        break;
    }
}

std::string LLMTransport::sendLLMRequest(const std::string& userMessage) {
    VASDK_CTRACK_NAME("LLM::sendRequest");
    try {
        std::string endpointValidationError;
        if (!normalizeAndValidateEndpoint(_config, endpointValidationError)) {
            fireOnError(static_cast<int>(LLMErrorCode::InvalidEndpoint), endpointValidationError);
            spdlog::error("[LLMTransport] Invalid endpoint configuration: {}", endpointValidationError);
            return "";
        }

        spdlog::debug("[LLMTransport] Preparing LLM request for provider {}, message: {}",
                      static_cast<int>(_provider), userMessage);

        std::string payload = buildRequestPayload(userMessage);

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json";

        applyProviderAuthHeaders(headers);
        for (const auto& [key, value] : _config.headers) {
            headers[key] = value;
        }

        spdlog::debug("[LLMTransport] Sending request to {}", _config.endpoint);
        spdlog::debug("[LLMTransport] Payload: {}", payload);

        auto t0 = std::chrono::steady_clock::now();
        std::string streamedText;
        std::string sseLineBuffer;
        bool sawSseData = false;

        const auto emitPartialFrame = [this](const std::string& delta) {
            if (delta.empty()) return;
            auto partialFrame = std::make_shared<transport::TextFrame>();
            partialFrame->text = delta;
            partialFrame->isFinal = false;
            partialFrame->timestamp = transport::Frame::getCurrentTimestamp();
            partialFrame->sequenceId = _nextSequenceId++;
            partialFrame->sourceId = getName();
            enqueueReceivedFrame(partialFrame);
        };

        const auto processSseLine = [this, &streamedText, &sawSseData, &emitPartialFrame](const std::string& rawLine) {
            std::string line = rawLine;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) return;
            if (line.rfind("data:", 0) != 0) return;
            sawSseData = true;

            std::string data = line.substr(5);
            size_t firstNonSpace = 0;
            while (firstNonSpace < data.size() &&
                   std::isspace(static_cast<unsigned char>(data[firstNonSpace])))
                ++firstNonSpace;
            data.erase(0, firstNonSpace);

            bool isDone = false;
            std::string delta;
            if (parseStreamingData(data, delta, isDone) && !delta.empty()) {
                streamedText += delta;
                emitPartialFrame(delta);
            }
        };

        va::network::HttpResponse resp;
        if (_config.streaming) {
            resp = httpPost(_config.endpoint, headers, payload, _config.timeoutMs,
                            [&sseLineBuffer, &processSseLine](const std::string& chunk) {
                                sseLineBuffer += chunk;
                                size_t pos;
                                while ((pos = sseLineBuffer.find('\n')) != std::string::npos) {
                                    std::string line = sseLineBuffer.substr(0, pos);
                                    sseLineBuffer.erase(0, pos + 1);
                                    processSseLine(line);
                                }
                            });
            if (!sseLineBuffer.empty()) {
                processSseLine(sseLineBuffer);
                sseLineBuffer.clear();
            }
        } else {
            resp = httpPost(_config.endpoint, headers, payload, _config.timeoutMs);
        }

        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        _apiRoundtripSampler.observe(ms);

        if (resp.error) return "";

        spdlog::debug("[LLMTransport] Received response: {}", resp.body);

        if (_config.streaming && !streamedText.empty()) {
            // Signal end-of-stream to LLMProcessor. Empty text + isFinal=true is the
            // terminal sentinel; LLMProcessor reconstructs full text from its buffer.
            auto terminalFrame = std::make_shared<transport::TextFrame>();
            terminalFrame->isFinal = true;
            terminalFrame->timestamp = transport::Frame::getCurrentTimestamp();
            terminalFrame->sequenceId = _nextSequenceId++;
            terminalFrame->sourceId = getName();
            enqueueReceivedFrame(terminalFrame);
            return streamedText;
        }

        // A stream that carried no content (e.g. max_tokens spent entirely on reasoning) must
        // not fall through to parseResponse: the body is SSE text, not a JSON document.
        // Bodies without SSE lines still do, for servers that ignore "stream": true.
        if (_config.streaming && sawSseData) {
            fireOnError(static_cast<int>(LLMErrorCode::EmptyResponse),
                        "LLM stream ended without any content");
            return "";
        }

        if (resp.body.empty()) {
            fireOnError(static_cast<int>(LLMErrorCode::EmptyResponse), "Empty response from LLM API");
            return "";
        }

        return parseResponse(resp.body);

    } catch (const std::exception& e) {
        fireOnError(static_cast<int>(LLMErrorCode::RequestFailed), std::string("LLM request failed: ") + e.what());
        return "";
    }
}

std::string LLMTransport::buildRequestPayload(const std::string& userMessage) {
    json payload;
    payload["model"] = _config.model;
    payload["temperature"] = _config.temperature;
    payload["max_tokens"] = _config.maxTokens;
    json userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = userMessage;
    payload["messages"] = json::array();
    payload["messages"].push_back(userMsg);
    return payload.dump();
}

std::string LLMTransport::parseResponse(const std::string& response) {
    try {
        [[maybe_unused]] auto parsed = json::parse(response);
        return "";
    } catch (const std::exception& e) {
        fireOnError(static_cast<int>(LLMErrorCode::ResponseParseFailed), std::string("Failed to parse LLM response: ") + e.what());
        return "";
    }
}

bool LLMTransport::parseStreamingData(const std::string& data,
                                      std::string& deltaText,
                                      bool& isDone) {
    deltaText.clear();
    isDone = (data == "[DONE]");
    return isDone;
}

} // namespace llm
} // namespace va
