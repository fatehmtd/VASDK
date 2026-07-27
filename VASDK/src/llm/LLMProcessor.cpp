#include <VASDK/llm/LLMProcessor.h>
#include <VASDK/llm/ToolDispatchProcessor.h>
#include <VASDK/profiling/CTrack.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <regex>

namespace va {
namespace llm {

LLMProcessor::LLMProcessor(std::shared_ptr<LLMTransport> transport)
    : pipeline::FrameProcessor(), _transport(transport) {
}

LLMProcessor::~LLMProcessor() {
    stop();
    release();
}

bool LLMProcessor::initialize() {
    if (_initialized) {
        return true;
    }

    if (!_transport) {
        spdlog::error("LLMProcessor: No transport provided");
        return false;
    }

    if (!_transport->initialize()) {
        spdlog::error("LLMProcessor: Failed to initialize transport");
        return false;
    }
    else {
        spdlog::info("[LLMProcessor] Transport initialized successfully");
    }

    if (_metrics.config.enabled && _collector) {
        using namespace va::metrics;
        _metrics.ttft.initialize(_collector,
            MetricDescriptor{ "llm_ttft_ms", MetricType::Sampler,
                              "Time to first LLM token (ms)",
                              _metrics.config.ttftWindow });
        _metrics.tokens.initialize(_collector,
            MetricDescriptor{ "llm_tokens_total", MetricType::Counter,
                              "Total LLM response tokens/chunks received" });
        _metrics.requests.initialize(_collector,
            MetricDescriptor{ "llm_requests_total", MetricType::Counter,
                              "Total LLM requests sent" });
        if (_metrics.config.ttftAlertMs > 0.0) {
            _metrics.ttftAlert.initialize(_collector,
                MetricDescriptor{ "llm_ttft_alert_ms", MetricType::Gauge,
                                  "TTFT alert threshold (ms)" });
            _metrics.ttftAlert.set(_metrics.config.ttftAlertMs);
        }
    }

    _transport->onFrameReceived = [this](std::shared_ptr<transport::Frame> f) {
        onTransportFrameReceived(f);
    };

    _initialized = true;
    return true;
}

bool LLMProcessor::start() {
    spdlog::info("[LLMProcessor] Starting processor");
    if (!pipeline::FrameProcessor::start()) {
        spdlog::error("[LLMProcessor] Failed to start base FrameProcessor");
        return false;
    }
    if (!_initialized) {
        spdlog::info("[LLMProcessor] Processor not initialized, initializing now");
        if (!initialize()) {
            spdlog::error("[LLMProcessor] Failed to initialize processor");
            return false;
        }
    }

    if (!_transport->start()) {
        spdlog::error("[LLMProcessor] Failed to start transport");
        return false;
    }
    else {
        spdlog::info("[LLMProcessor] Transport started successfully");
    }

    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogStop = false;
        _watchdogArmed = false;
    }
    _finalWatchdogThread = std::thread(&LLMProcessor::finalWatchdogLoop, this);

    spdlog::info("[LLMProcessor] Processor started");

    return true;
}

void LLMProcessor::stop() {
    if (!_running) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogStop = true;
        _watchdogArmed = false;
    }
    _watchdogCv.notify_all();
    if (_finalWatchdogThread.joinable()) _finalWatchdogThread.join();

    if (_transport) {
        _transport->stop();
    }

    pipeline::FrameProcessor::stop();
}

void LLMProcessor::release() {
    stop();

    if (_transport) {
        _transport->onFrameReceived = nullptr;
        _transport->release();
    }

    _initialized = false;
}

void LLMProcessor::setConfig(const LLMConfig& config) {
    if (_transport) {
        _transport->setConfig(config);
    }
}

const LLMConfig& LLMProcessor::getConfig() const {
    if (!_transport) {
        throw std::runtime_error("LLMProcessor::getConfig() called with no transport set");
    }
    return _transport->getConfig();
}

void LLMProcessor::setHistoryEnabled(bool enabled) {
    if (_transport) {
        _transport->setHistoryEnabled(enabled);
    }
}

void LLMProcessor::clearHistory() {
    if (_transport) {
        _transport->clearHistory();
    }
}

void LLMProcessor::setMaxHistoryLength(size_t maxMessages) {
    if (_transport) {
        _transport->setMaxHistoryLength(maxMessages);
    }
}

const std::vector<ConversationMessage>& LLMProcessor::getHistory() const {
    return _transport->getHistory();
}

bool LLMProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    if (!frame) {
        return false;
    }

    auto type = frame->getType();

    switch(type) {
        case transport::FrameType::AGENT_STATE:
        case transport::FrameType::TRANSCRIPTION:
        case transport::FrameType::TURN_STATE:
        case transport::FrameType::TOOL_CALL:
        case transport::FrameType::TOOL_RESULT:
            return true;
        default:
            break;
    }

    return false;
}

void LLMProcessor::processFrame(std::shared_ptr<transport::Frame> frame,
                                 pipeline::FlowDirection direction) {
    if (!frame) {
        return;
    }

    if (direction == pipeline::FlowDirection::DOWNSTREAM) {
        processDownstream(frame);
    }
    else {
        if (_toolDispatcher && frame->getType() == transport::FrameType::TOOL_RESULT) {
            auto resultFrame = std::dynamic_pointer_cast<transport::ToolResultFrame>(frame);
            if (resultFrame) {
                {
                    std::lock_guard<std::mutex> lk(_toolResultsMutex);
                    if (_toolBatchGeneration != _currentGeneration.load()) {
                        spdlog::debug("[LLMProcessor] Dropping stale tool result for '{}' (barge-in occurred)",
                            resultFrame->toolName);
                        return;
                    }
                }
                bool allDone = false;
                std::vector<std::pair<std::string,std::string>> snapshot;
                {
                    std::lock_guard<std::mutex> lk(_toolResultsMutex);
                    _collectedToolResults.emplace_back(resultFrame->toolName,
                                                       resultFrame->resultJson);
                    allDone = (static_cast<int>(_collectedToolResults.size())
                               >= _pendingToolCount.load());
                    if (allDone) snapshot = _collectedToolResults;
                }
                if (allDone) {
                    if (_toolCallDepth >= kMaxToolCallDepth) {
                        spdlog::warn("[LLMProcessor] Tool call depth limit ({}) reached - dropping batch",
                            kMaxToolCallDepth);
                        std::string lastText;
                        {
                            std::lock_guard<std::mutex> lk2(_toolResultsMutex);
                            _pendingToolCount.store(0);
                            _collectedToolResults.clear();
                            lastText = _lastToolBatchText;
                        }
                        _toolCallDepth = 0;
                        // Degrade to speaking the text suppressed alongside the
                        // dropped batch rather than ending the turn in silence.
                        if (!lastText.empty()) {
                            auto textOut       = std::make_shared<transport::TextFrame>();
                            textOut->text      = lastText;
                            textOut->isFinal   = true;
                            textOut->timestamp = transport::Frame::getCurrentTimestamp();
                            textOut->sourceId  = getName();
                            pushDownstream(textOut);
                        }
                        return;
                    }
                    ++_toolCallDepth;
                    // Reset before injecting so the next tool batch starts with a clean count.
                    {
                        std::lock_guard<std::mutex> lk2(_toolResultsMutex);
                        _pendingToolCount.store(0);
                        _collectedToolResults.clear();
                    }
                    _transport->injectToolResults(snapshot);
                }
                return;
            }
        }
        pushUpstream(frame);
    }
}

void LLMProcessor::setToolDispatcher(std::shared_ptr<ToolDispatchProcessor> dispatcher) {
    _toolDispatcher = std::move(dispatcher);
}

std::pair<std::string, std::string> LLMProcessor::effectiveReasoningTags() const {
    if (_reasoningTagsOverridden) return { _reasoningTagOpen, _reasoningTagClose };
    if (_transport) {
        // Query the format's PromptTemplate subclass directly rather than a persistent
        // instance - the transport may not have one attached (e.g. LlamaCppTransport
        // bypasses PromptTemplate; "GGUF carries its own embedded template") even
        // though getPromptChatFormat() still reports which format it targets.
        if (auto tmpl = PromptTemplate::create(_transport->getPromptChatFormat())) {
            return tmpl->reasoningTags();
        }
    }
    return { "<think>", "</think>" }; // no transport/format (shouldn't happen) - historical default
}

namespace {
// The model is expected to emit a JSON object, but malformed output (truncated
// or mismatched brackets) is not uncommon with smaller/quantized local models.
// Rather than speaking the raw JSON blob to the user, try to salvage just the
// "text" field with a regex before giving up entirely.
std::string extractTextFallback(const std::string& src) {
    static const std::regex textFieldRe(R"re("text"\s*:\s*"((?:[^"\\]|\\.)*)")re");
    std::smatch m;
    if (std::regex_search(src, m, textFieldRe)) {
        try {
            return nlohmann::json::parse("\"" + m[1].str() + "\"").get<std::string>();
        } catch (...) {
            // fall through to raw text below
        }
    }
    return src;
}

std::string repairJsonBrackets(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::vector<char> stack; // expected closer for each currently-open bracket
    bool inString = false;
    bool escape = false;
    for (char c : s) {
        if (inString) {
            if (escape) { out += c; escape = false; continue; }
            if (c == '\\') { out += c; escape = true; continue; }
            if (c == '"') { inString = false; out += c; continue; }
            out += c;
            continue;
        }
        if (c == '"') { inString = true; out += c; continue; }
        if (c == '{') { stack.push_back('}'); out += c; continue; }
        if (c == '[') { stack.push_back(']'); out += c; continue; }
        if (c == '}' || c == ']') {
            if (!stack.empty() && stack.back() == c) {
                stack.pop_back();
                out += c;
            }
            // else: stray/mismatched closer - drop it rather than emit invalid JSON
            continue;
        }
        out += c;
    }
    if (inString) {
        // Truncated mid-string - drop a dangling trailing backslash (can't escape
        // nothing) then close the string so the value up to the cut is preserved.
        if (escape && !out.empty()) out.pop_back();
        out += '"';
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) out += *it;
    return out;
}
} // namespace

LLMProcessor::StructuredResponse LLMProcessor::parseStructuredResponse(
        const std::string& raw,
        const std::string& reasoningTagOpen,
        const std::string& reasoningTagClose) {
    StructuredResponse resp;

    // Strip reasoning blocks emitted by "thinking" models (e.g. Qwen3, DeepSeek-R1
    // use <think>...</think>; other models use different wrapper tags entirely, or
    // none - hence these are caller-supplied rather than hardcoded here). The actual
    // response follows the closing tag.
    std::string stripped = raw;
    if (!reasoningTagOpen.empty() && !reasoningTagClose.empty()) {
        size_t thinkStart = stripped.find(reasoningTagOpen);
        while (thinkStart != std::string::npos) {
            size_t thinkEnd = stripped.find(reasoningTagClose, thinkStart);
            if (thinkEnd == std::string::npos) break;
            stripped.erase(thinkStart, thinkEnd + reasoningTagClose.size() - thinkStart);
            thinkStart = stripped.find(reasoningTagOpen);
        }
        // Trim leading whitespace left after stripping
        size_t first = stripped.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) stripped = stripped.substr(first);
    }
    const std::string& src = stripped;

    // Find the outermost JSON object
    size_t jsonStart = src.find('{');
    if (jsonStart == std::string::npos) {
        resp.text = src;
        return resp;
    }
    size_t depth = 0, jsonEnd = std::string::npos;
    for (size_t i = jsonStart; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) { jsonEnd = i; break; }
        }
    }

    // Populates resp from a parsed JSON object; returns false (without mutating
    // resp) if jsonText doesn't parse or isn't a JSON object.
    auto tryParseAndFill = [&](const std::string& jsonText) -> bool {
        nlohmann::json jsonPayload;
        try {
            jsonPayload = nlohmann::json::parse(jsonText);
        } catch (...) {
            return false;
        }
        if (!jsonPayload.is_object()) return false;

        if (jsonPayload.contains("text") && jsonPayload["text"].is_string())
            resp.text = jsonPayload["text"].get<std::string>();

        if (jsonPayload.contains("isExpecting") && jsonPayload["isExpecting"].is_boolean())
            resp.isExpecting = jsonPayload["isExpecting"].get<bool>();

        if (jsonPayload.contains("choices") && jsonPayload["choices"].is_array()) {
            for (auto& c : jsonPayload["choices"]) {
                if (c.contains("text") && c["text"].is_string()) {
                    transport::TextFrame::Choice choice;
                    choice.text = c["text"].get<std::string>();
                    resp.choices.push_back(std::move(choice));
                }
            }
        }

        // New "tools" array format
        if (jsonPayload.contains("tools") && jsonPayload["tools"].is_array()) {
            for (auto& t : jsonPayload["tools"]) {
                if (t.contains("tool") && t["tool"].is_string() &&
                    t.contains("args"))
                    resp.tools.push_back({ t["tool"].get<std::string>(), t["args"].dump() });
            }
        }
        // Legacy single tool/args fallback
        else if (jsonPayload.contains("tool") && jsonPayload["tool"].is_string() &&
                 jsonPayload.contains("args") && jsonPayload["args"].is_object()) {
            resp.tools.push_back({ jsonPayload["tool"].get<std::string>(), jsonPayload["args"].dump() });
        }
        return true;
    };

    if (jsonEnd != std::string::npos &&
        tryParseAndFill(src.substr(jsonStart, jsonEnd - jsonStart + 1))) {
        return resp;
    }

    // Strict parse failed (or the naive brace counter above mislocated the end,
    // e.g. because of a stray extra '}') - attempt to repair bracket/brace
    // mismatches across the rest of the string and retry before giving up on
    // the tool calls entirely.
    std::string repaired = repairJsonBrackets(src.substr(jsonStart));
    if (tryParseAndFill(repaired)) {
        spdlog::warn("[LLMProcessor] Recovered malformed structured response via bracket repair: '{}'", src);
        return resp;
    }

    // Still unrecoverable - salvage just the "text" field via regex so we at
    // least don't speak the raw JSON blob out loud. Any tool calls are lost.
    spdlog::warn("[LLMProcessor] Failed to parse structured response, salvaging text only: '{}'", src);
    resp.text = extractTextFallback(src);
    return resp;
}

void LLMProcessor::processDownstream(std::shared_ptr<transport::Frame> frame) {
    if (frame->getType() == transport::FrameType::AGENT_STATE) {
        auto agentFrame = std::dynamic_pointer_cast<transport::AgentStateFrame>(frame);
        if (agentFrame && handleAgentStateTransition(agentFrame)) {
            return; // handler already pushed the frame downstream and dispatched a request
        }
        pushDownstream(frame);
        return;
    }

    if (frame->getType() == transport::FrameType::TURN_STATE) {
        auto turnFrame = std::dynamic_pointer_cast<transport::TurnStateFrame>(frame);
        if (turnFrame &&
            turnFrame->actor == transport::TurnStateFrame::TurnActor::USER &&
            turnFrame->state == transport::TurnStateFrame::TurnState::END &&
            _transport && _transport->isConnected()) {

            std::string stagedFinal;
            float       stagedConf = 0.0f;
            bool        alreadyStaged = false;
            {
                std::lock_guard<std::mutex> lock(_pendingTurnMutex);
                if (_pendingTurnEnd && !_accumulatedFinalText.empty()) {
                    stagedFinal = _accumulatedFinalText;
                    stagedConf  = _accumulatedFinalConf;
                    _pendingTurnEnd = false;
                    _accumulatedFinalText.clear();
                    alreadyStaged = true;
                }
            }
            if (alreadyStaged) {
                cancelFinalWatchdog();
                spdlog::info("[LLMProcessor] TurnStateFrame END - final already staged; "
                    "firing LLM immediately: \"{}\" (conf={:.2f})",
                    stagedFinal.substr(0, std::min<size_t>(60, stagedFinal.size())),
                    stagedConf);
                pushDownstream(frame);
                dispatchLLMRequest(stagedFinal, stagedConf);
                return;
            }
        }
    }

    if (!shouldProcess(frame)) {
        pushDownstream(frame);
        return;
    }

    auto transcriptionFrame = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame);
    if (transcriptionFrame && !transcriptionFrame->isFinal) {
        if (!transcriptionFrame->transcriptionResult.text.empty()) {
            std::lock_guard<std::mutex> lock(_partialCacheMutex);
            _latestPartialText = transcriptionFrame->transcriptionResult.text;
            _latestPartialConfidence = transcriptionFrame->transcriptionResult.confidence;
        }
        pushDownstream(frame);
        return;
    }

    spdlog::info("[LLMProcessor] Processing frame for LLM, type: {}, transport: {}, transport connected: {}",
        static_cast<int>(frame->getType()),
        _transport ? _transport->getName() : "null",
        _transport ? (_transport->isConnected() ? "yes" : "no") : "N/A");

    {
        bool shouldFire = false;
        bool emptyFinal = false;
        std::string textToFire;
        float confToFire = 0.0f;
        auto finalTF = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame);
        {
            std::lock_guard<std::mutex> lock(_pendingTurnMutex);
            if (_pendingTurnEnd && finalTF) {
                const std::string& incoming = finalTF->transcriptionResult.text;
                if (!_accumulatedFinalText.empty() && !incoming.empty()) {
                    _accumulatedFinalText += " ";
                }
                _accumulatedFinalText += incoming;
                if (finalTF->transcriptionResult.confidence > _accumulatedFinalConf) {
                    _accumulatedFinalConf = finalTF->transcriptionResult.confidence;
                }
                textToFire = _accumulatedFinalText;
                confToFire = _accumulatedFinalConf;
                shouldFire = !textToFire.empty();
                _pendingTurnEnd = false;
                if (!shouldFire) {
                    // The final we were waiting for arrived but carried no text.
                    // There is nothing left to wait for, so fall back to the
                    // cached partial now; waiting for the watchdog here loses the
                    // turn whenever the user re-speaks before it expires (the
                    // interrupt path discards the pending fallback).
                    emptyFinal = true;
                    textToFire = _pendingFallbackText;
                    confToFire = _pendingFallbackConf;
                }
            }
        }
        if (shouldFire) {
            cancelFinalWatchdog();
            spdlog::info("[LLMProcessor] Final transcription after turn end - firing LLM: \"{}\" "
                "(conf={:.2f})",
                textToFire.substr(0, std::min<size_t>(60, textToFire.size())),
                confToFire);
            pushDownstream(frame);
            dispatchLLMRequest(textToFire, confToFire);
            return;
        }
        if (emptyFinal) {
            cancelFinalWatchdog();
            if (!textToFire.empty()) {
                int wordCount = 0;
                bool inWord = false;
                for (char c : textToFire) {
                    if (c == ' ' || c == '\t' || c == '\n') { inWord = false; }
                    else if (!inWord) { ++wordCount; inWord = true; }
                }
                LLMFireContext ctx{ textToFire, confToFire, 0.0f, wordCount };
                LLMFireDecision decision = _firePolicy->evaluate(ctx);
                if (decision == LLMFireDecision::FIRE_NOW) {
                    spdlog::warn("[LLMProcessor] Final transcription after turn end was empty; "
                        "firing fallback partial: \"{}\" (conf={:.2f}, words={})",
                        textToFire.substr(0, std::min<size_t>(60, textToFire.size())),
                        confToFire, wordCount);
                    pushDownstream(frame);
                    dispatchLLMRequest(textToFire, confToFire);
                    return;
                }
                spdlog::warn("[LLMProcessor] Final transcription after turn end was empty; "
                    "fire policy suppressed partial fallback: \"{}\"",
                    textToFire.substr(0, std::min<size_t>(60, textToFire.size())));
            }
            else {
                spdlog::warn("[LLMProcessor] Final transcription after turn end was empty "
                    "and no fallback partial is available; not firing LLM");
            }
            pushDownstream(frame);
            return;
        }
    }

    if (_transport && _transport->isConnected()) {
        if (_partialAlreadySent) {
            spdlog::info("[LLMProcessor] Skipping FINAL transcription - LLM already fired from partial (TRANSCRIPTION_READY)");
            _partialAlreadySent = false;
            pushDownstream(frame);
            return;
        }

        if (auto finalTF = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame)) {
            const std::string& text = finalTF->transcriptionResult.text;
            if (!text.empty()) {
                spdlog::info("[LLMProcessor] Final transcription arrived before turn-end signal "
                    "- pre-staging for USER_STOPPED_SPEAKING: \"{}\"",
                    text.substr(0, std::min<size_t>(60, text.size())));
                {
                    std::lock_guard<std::mutex> lock(_pendingTurnMutex);
                    _pendingTurnEnd = true;
                    _pendingFallbackText = text;
                    _pendingFallbackConf = finalTF->transcriptionResult.confidence;
                    _accumulatedFinalText = text;
                    _accumulatedFinalConf = finalTF->transcriptionResult.confidence;
                }
                startFinalWatchdog();
            }
        }
        pushDownstream(frame);
        return;
    }
    else {
        spdlog::warn("[LLMProcessor] Transport not connected, passing frame through");
        pushDownstream(frame);
    }
}

void LLMProcessor::onTransportFrameReceived(std::shared_ptr<transport::Frame> frame) {
    if (_dispatchGeneration.load() != _currentGeneration.load()) {
        _streamingBuffer.clear();
        return;
    }

    if (!_firstTokenReceived) {
        _firstTokenReceived = true;
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - _llmRequestStart).count();
        _metrics.ttft.observe(ms);
    }
    _metrics.tokens.increment();

    if (frame->getType() == transport::FrameType::TEXT) {
        auto textFrame = std::dynamic_pointer_cast<transport::TextFrame>(frame);
        if (textFrame) {
            if (!textFrame->isFinal) {
                _streamingBuffer.push_back(textFrame);
                return;
            }

            std::string fullText;
            for (const auto& chunk : _streamingBuffer) fullText += chunk->text;
            fullText += textFrame->text;
            _streamingBuffer.clear();

            auto [reasoningOpen, reasoningClose] = effectiveReasoningTags();
            auto resp = parseStructuredResponse(fullText, reasoningOpen, reasoningClose);

            // Canonical (sorted) view of this response's tool batch. A follow-up
            // response (_toolCallDepth > 0) that re-requests the exact batch whose
            // results were just injected is a self-imitation loop, not a real
            // follow-up - dispatching it again would spin until the depth cap with
            // the answer never spoken. Treat its text as the final answer instead.
            std::vector<std::pair<std::string,std::string>> batch;
            bool repeatedBatch = false;
            if (resp.hasTool()) {
                batch.reserve(resp.tools.size());
                for (const auto& tc : resp.tools)
                    batch.emplace_back(tc.toolName, tc.argsJson);
                std::sort(batch.begin(), batch.end());
                std::lock_guard<std::mutex> lk(_toolResultsMutex);
                repeatedBatch = (_toolCallDepth > 0 && batch == _lastToolBatch);
            }
            if (repeatedBatch) {
                spdlog::warn("[LLMProcessor] Model re-requested the identical tool batch "
                    "after receiving its results - treating text as the final answer");
                _toolCallDepth = 0;
            }

            // Only emit speech text when this turn has no tool calls.
            // When both text and tools are present the model is violating the
            // "call tools before speaking" contract: the follow-up response
            // (after tool results are injected) will carry the authoritative
            // speech text, so emitting here would cause a duplicate TTS utterance.
            // Exception: a repeated batch means no follow-up is coming - speak now.
            if (!resp.text.empty() && (!resp.hasTool() || repeatedBatch)) {
                auto textOut         = std::make_shared<transport::TextFrame>();
                textOut->text        = resp.text;
                textOut->isFinal     = true;
                textOut->isExpecting = resp.isExpecting;
                textOut->choices     = resp.choices;
                textOut->timestamp   = textFrame->timestamp;
                textOut->sourceId    = textFrame->sourceId;
                pushDownstream(textOut);
            }

            if (_toolDispatcher && resp.hasTool() && !repeatedBatch) {
                {
                    std::lock_guard<std::mutex> lk(_toolResultsMutex);
                    _collectedToolResults.clear();
                    _pendingToolCount.store(static_cast<int>(resp.tools.size()));
                    _toolBatchGeneration = _currentGeneration.load();
                    _lastToolBatch     = std::move(batch);
                    _lastToolBatchText = resp.text;
                }
                for (auto& tc : resp.tools) {
                    auto toolFrame      = std::make_shared<transport::ToolCallFrame>();
                    toolFrame->toolName = tc.toolName;
                    toolFrame->argsJson = tc.argsJson;
                    toolFrame->timestamp = transport::Frame::getCurrentTimestamp();
                    toolFrame->sourceId  = getName();
                    _toolDispatcher->handleToolCall(toolFrame);
                }
            }
            return;
        }
    }

    // Native ToolCallFrame from LlamaCppTransport (LFM2.5 <|tool_call_start|> path).
    // Register it in the batch-tracking state so the upstream result-collection
    // logic in processFrame(UPSTREAM) works correctly, then forward it downstream
    // to ToolDispatchProcessor.
    if (_toolDispatcher && frame->getType() == transport::FrameType::TOOL_CALL) {
        {
            std::lock_guard<std::mutex> lk(_toolResultsMutex);
            // First native tool call frame in this batch - reset collection state.
            if (_toolBatchGeneration != _currentGeneration.load()) {
                _collectedToolResults.clear();
                _pendingToolCount.store(0);
                _toolBatchGeneration = _currentGeneration.load();
            }
            _pendingToolCount.fetch_add(1);
        }
    }

    pushDownstream(frame);
}

bool LLMProcessor::handleAgentStateTransition(std::shared_ptr<transport::AgentStateFrame> agentFrame) {
    switch (agentFrame->transition) {
    case transport::AgentStateTransition::BARGE_IN_INTERRUPT:
    case transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING:
        handleInterruptTransition(agentFrame->transition);
        return false;
    case transport::AgentStateTransition::USER_STARTED_SPEAKING:
        handleUserStartedSpeakingTransition();
        return false;
    case transport::AgentStateTransition::TRANSCRIPTION_READY:
        handleTranscriptionReadyTransition();
        return false;
    case transport::AgentStateTransition::USER_STOPPED_SPEAKING:
        return handleUserStoppedSpeakingTransition(agentFrame);
    default:
        return false;
    }
}

void LLMProcessor::handleInterruptTransition(transport::AgentStateTransition t) {
    spdlog::info("[LLMProcessor] Interrupt received ({}); cancelling LLM",
        transport::toString(t));
    if (_transport) _transport->cancel();
    _partialAlreadySent = false;
    {
        std::lock_guard<std::mutex> lock(_partialCacheMutex);
        _latestPartialText.clear();
        _latestPartialConfidence = 0.0f;
    }
    cancelFinalWatchdog();
    {
        std::lock_guard<std::mutex> lock(_pendingTurnMutex);
        _pendingTurnEnd = false;
        _accumulatedFinalText.clear();
    }
    ++_currentGeneration;
    _streamingBuffer.clear();
    {
        std::lock_guard<std::mutex> lk(_toolResultsMutex);
        _pendingToolCount.store(0);
        _collectedToolResults.clear();
        _lastToolBatch.clear();
        _lastToolBatchText.clear();
        // Deliberately leave _toolBatchGeneration at its old value so it now
        // mismatches the bumped _currentGeneration - this is what makes the
        // staleness check in processFrame(UPSTREAM, TOOL_RESULT) drop results
        // for the batch we just interrupted instead of treating each straggler
        // as its own complete (size==0) batch and firing a spurious LLM request.
    }
    _toolCallDepth = 0;
}

void LLMProcessor::handleUserStartedSpeakingTransition() {
    {
        std::lock_guard<std::mutex> lock(_partialCacheMutex);
        _latestPartialText.clear();
        _latestPartialConfidence = 0.0f;
    }
    cancelFinalWatchdog();
    {
        std::lock_guard<std::mutex> lock(_pendingTurnMutex);
        _pendingTurnEnd = false;
        _accumulatedFinalText.clear();
    }
}

void LLMProcessor::handleTranscriptionReadyTransition() {
    if (!_transport || !_transport->isConnected()) return;

    std::string partial;
    float partialConf;
    {
        std::lock_guard<std::mutex> lock(_partialCacheMutex);
        partial = _latestPartialText;
        partialConf = _latestPartialConfidence;
    }
    constexpr float kDefinitiveTurnConf = 1.0f;

    int wordCount = 0;
    bool inWord = false;
    for (char c : partial) {
        if (c == ' ' || c == '\t' || c == '\n') { inWord = false; }
        else if (!inWord) { ++wordCount; inWord = true; }
    }

    LLMFireContext ctx{ partial, partialConf, kDefinitiveTurnConf, wordCount };
    LLMFireDecision decision = _firePolicy->evaluate(ctx);

    if (decision == LLMFireDecision::FIRE_NOW) {
        spdlog::info("[LLMProcessor] {} FIRE_NOW on TRANSCRIPTION_READY - partial: \"{}\" "
            "(partialConf={:.2f}, words={})",
            _firePolicy->name(),
            partial.substr(0, std::min<size_t>(60, partial.size())),
            partialConf, wordCount);
        _partialAlreadySent = true;
        _llmRequestStart = std::chrono::steady_clock::now();
        _firstTokenReceived = false;
        _toolCallDepth = 0;
        {
            std::lock_guard<std::mutex> lk(_toolResultsMutex);
            _lastToolBatch.clear();
            _lastToolBatchText.clear();
        }
        _metrics.requests.increment();
        auto syntheticFrame = std::make_shared<transport::TranscriptionFrame>();
        syntheticFrame->isFinal = true;
        syntheticFrame->transcriptionResult.text = partial;
        syntheticFrame->transcriptionResult.confidence = 1.0f;
        syntheticFrame->timestamp = transport::Frame::getCurrentTimestamp();
        syntheticFrame->sourceId = getName();
        _transport->sendFrame(syntheticFrame);
    }
    else {
        spdlog::info("[LLMProcessor] {} WAIT_FOR_FINAL on TRANSCRIPTION_READY "
            "(partialConf={:.2f}, words={})",
            _firePolicy->name(), partialConf, wordCount);
    }
}

bool LLMProcessor::handleUserStoppedSpeakingTransition(std::shared_ptr<transport::AgentStateFrame> agentFrame) {
    if (!_transport || !_transport->isConnected()) return false;

    std::string partial;
    float partialConf;
    {
        std::lock_guard<std::mutex> lock(_partialCacheMutex);
        partial = _latestPartialText;
        partialConf = _latestPartialConfidence;
    }

    std::string stagedFinal;
    float       stagedConf = 0.0f;
    bool        alreadyStaged = false;
    {
        std::lock_guard<std::mutex> lock(_pendingTurnMutex);
        if (_pendingTurnEnd && !_accumulatedFinalText.empty()) {
            stagedFinal = _accumulatedFinalText;
            stagedConf = _accumulatedFinalConf;
            _pendingTurnEnd = false;
            _accumulatedFinalText.clear();
            alreadyStaged = true;
        }
    }

    if (alreadyStaged) {
        cancelFinalWatchdog();
        spdlog::info("[LLMProcessor] USER_STOPPED_SPEAKING - final already staged; "
            "firing LLM immediately: \"{}\" (conf={:.2f})",
            stagedFinal.substr(0, std::min<size_t>(60, stagedFinal.size())),
            stagedConf);
        pushDownstream(agentFrame);
        dispatchLLMRequest(stagedFinal, stagedConf);
        return true;
    }

    spdlog::info("[LLMProcessor] USER_STOPPED_SPEAKING - "
        "waiting for final transcription (fallback partial: \"{}\" conf={:.2f})",
        partial.substr(0, std::min<size_t>(60, partial.size())),
        partialConf);
    {
        std::lock_guard<std::mutex> lock(_pendingTurnMutex);
        _pendingTurnEnd = true;
        _pendingFallbackText = partial;
        _pendingFallbackConf = partialConf;
        _accumulatedFinalText.clear();
        _accumulatedFinalConf = 0.0f;
    }
    startFinalWatchdog();
    return false;
}

void LLMProcessor::dispatchLLMRequest(const std::string& text, float confidence) {
    VASDK_CTRACK_NAME("LLM::dispatchRequest");
    if (!_transport || !_transport->isConnected()) {
        spdlog::warn("[LLMProcessor] dispatchLLMRequest: transport not connected, skipping");
        return;
    }
    _llmRequestStart = std::chrono::steady_clock::now();
    _firstTokenReceived = false;
    _toolCallDepth = 0;
    {
        std::lock_guard<std::mutex> lk(_toolResultsMutex);
        _lastToolBatch.clear();
        _lastToolBatchText.clear();
    }
    _metrics.requests.increment();

    auto frame = std::make_shared<transport::TranscriptionFrame>();
    frame->isFinal = true;
    frame->transcriptionResult.text = text;
    frame->transcriptionResult.confidence = confidence;
    frame->timestamp = transport::Frame::getCurrentTimestamp();
    frame->sourceId = getName();
    _dispatchGeneration.store(_currentGeneration.load());
    _transport->sendFrame(frame);
}

void LLMProcessor::startFinalWatchdog() {
    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogArmed = true;
    }
    _watchdogCv.notify_one();
}

void LLMProcessor::cancelFinalWatchdog() {
    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogArmed = false;
    }
    _watchdogCv.notify_one();
}

void LLMProcessor::finalWatchdogLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(_watchdogMutex);
        _watchdogCv.wait(lock, [this] {
            return _watchdogArmed || _watchdogStop;
        });

        if (_watchdogStop) break;

        const auto timeout = std::chrono::milliseconds(_finalWatchdogTimeoutMs);
        const bool timedOut = !_watchdogCv.wait_for(lock, timeout, [this] {
            return !_watchdogArmed || _watchdogStop;
        });

        if (_watchdogStop) break;

        if (timedOut && _watchdogArmed) {
            _watchdogArmed = false;
            lock.unlock();

            std::string textToFire;
            float confToFire  = 0.0f;
            bool  isFinalText = false;
            {
                std::lock_guard<std::mutex> ptLock(_pendingTurnMutex);
                if (!_pendingTurnEnd) continue;
                _pendingTurnEnd = false;
                isFinalText = !_accumulatedFinalText.empty();
                textToFire  = isFinalText ? _accumulatedFinalText : _pendingFallbackText;
                confToFire  = isFinalText ? _accumulatedFinalConf  : _pendingFallbackConf;
                _accumulatedFinalText.clear();
            }

            if (!textToFire.empty()) {
                if (isFinalText) {
                    spdlog::info("[LLMProcessor] Watchdog timeout - final already staged; "
                        "firing LLM: \"{}\" (conf={:.2f})",
                        textToFire.substr(0, std::min<size_t>(60, textToFire.size())),
                        confToFire);
                    dispatchLLMRequest(textToFire, confToFire);
                } else {
                    int wordCount = 0;
                    bool inWord = false;
                    for (char c : textToFire) {
                        if (c == ' ' || c == '\t' || c == '\n') { inWord = false; }
                        else if (!inWord) { ++wordCount; inWord = true; }
                    }
                    LLMFireContext ctx{ textToFire, confToFire, 0.0f, wordCount };
                    LLMFireDecision decision = _firePolicy->evaluate(ctx);
                    if (decision == LLMFireDecision::FIRE_NOW) {
                        spdlog::warn("[LLMProcessor] Watchdog timeout - no final transcription "
                            "received; firing fallback partial: \"{}\" (conf={:.2f}, words={})",
                            textToFire.substr(0, std::min<size_t>(60, textToFire.size())),
                            confToFire, wordCount);
                        dispatchLLMRequest(textToFire, confToFire);
                    }
                    else {
                        spdlog::warn("[LLMProcessor] Watchdog timeout - fire policy suppressed "
                            "partial fallback (no final arrived): \"{}\"",
                            textToFire.substr(0, std::min<size_t>(60, textToFire.size())));
                    }
                }
            }
            else {
                spdlog::info("[LLMProcessor] Watchdog timeout - no partial or final text; "
                    "not firing LLM");
            }
        }
    }
}

void LLMProcessor::setMetricsConfig(const va::metrics::LLMMetricsConfig& config) {
    _metrics.config = config;
    pipeline::FrameProcessor::setMetricsConfig(
        static_cast<const va::metrics::ProcessorMetricsConfig&>(config));
}

} // namespace llm
} // namespace va
