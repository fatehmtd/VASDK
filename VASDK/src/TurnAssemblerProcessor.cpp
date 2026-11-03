#include "TurnAssemblerProcessor.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace va {
namespace pipeline {

namespace {

std::string preview(const std::string& text) { return text.substr(0, std::min<size_t>(60, text.size())); }

int countWords(const std::string& text) {
    int words = 0;
    bool inWord = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n') inWord = false;
        else if (!inWord) { ++words; inWord = true; }
    }
    return words;
}

} // namespace

TurnAssemblerProcessor::TurnAssemblerProcessor() = default;

TurnAssemblerProcessor::~TurnAssemblerProcessor() { stop(); }

bool TurnAssemblerProcessor::start() {
    if (!FrameProcessor::start()) return false;
    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogStop = false;
        _watchdogArmed = false;
    }
    _watchdogThread = std::thread([this] { finalWatchdogLoop(); });
    return true;
}

void TurnAssemblerProcessor::stop() {
    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogStop = true;
        _watchdogArmed = false;
    }
    _watchdogCv.notify_all();
    if (_watchdogThread.joinable()) _watchdogThread.join();
    FrameProcessor::stop();
}

bool TurnAssemblerProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
    const auto type = frame->getType();
    return type == transport::FrameType::TRANSCRIPTION || type == transport::FrameType::TURN_STATE ||
           type == transport::FrameType::AGENT_STATE;
}

void TurnAssemblerProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
    if (direction != FlowDirection::DOWNSTREAM) {
        pushUpstream(frame);
        return;
    }
    // The frame first: a turn it completes follows it.
    pushDownstream(frame);
    switch (frame->getType()) {
    case transport::FrameType::AGENT_STATE:
        handleAgentState(static_cast<const transport::AgentStateFrame&>(*frame));
        break;
    case transport::FrameType::TURN_STATE: {
        const auto& turn = static_cast<const transport::TurnStateFrame&>(*frame);
        if (turn.actor == transport::TurnStateFrame::TurnActor::USER &&
            turn.state == transport::TurnStateFrame::TurnState::END) {
            handleTurnStateEnd();
        }
        break;
    }
    case transport::FrameType::TRANSCRIPTION: {
        const auto& transcription = static_cast<const transport::TranscriptionFrame&>(*frame);
        if (transcription.isBackchannel || transcription.isFollowUp) break;   // not the user's turn
        if (!transcription.isFinal) {
            if (!transcription.transcriptionResult.text.empty()) {
                std::lock_guard<std::mutex> lock(_mutex);
                _latestPartialText = transcription.transcriptionResult.text;
                _latestPartialConfidence = transcription.transcriptionResult.confidence;
            }
            break;
        }
        handleFinal(transcription);
        break;
    }
    default:
        break;
    }
}

void TurnAssemblerProcessor::handleAgentState(const transport::AgentStateFrame& frame) {
    switch (frame.transition) {
    case transport::AgentStateTransition::BARGE_IN_INTERRUPT:
    case transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING:
        handleInterrupt(frame.transition);
        break;
    case transport::AgentStateTransition::USER_STARTED_SPEAKING: {
        std::lock_guard<std::mutex> lock(_mutex);
        _latestPartialText.clear();
        _latestPartialConfidence = 0.0f;
        cancelFinalWatchdog();
        keepUnsentTurn();
        break;
    }
    case transport::AgentStateTransition::BACKCHANNEL: {
        // The dismissed "mm-hmm" must not become the fallback text of the user's next turn.
        std::lock_guard<std::mutex> lock(_mutex);
        _latestPartialText.clear();
        _latestPartialConfidence = 0.0f;
        break;
    }
    case transport::AgentStateTransition::TRANSCRIPTION_READY:
        handleTranscriptionReady();
        break;
    case transport::AgentStateTransition::USER_STOPPED_SPEAKING:
        handleUserStoppedSpeaking();
        break;
    default:
        break;
    }
}

void TurnAssemblerProcessor::handleInterrupt(transport::AgentStateTransition transition) {
    std::lock_guard<std::mutex> lock(_mutex);
    const bool stillThinking = transition == transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING;
    const bool withinWindow =
        std::chrono::steady_clock::now() - _lastTurnAt <= std::chrono::milliseconds(_resumeMergeMs);
    if (_resumeMergeMs > 0 && !_lastTurnText.empty() && (stillThinking || withinWindow)) {
        spdlog::info("[TurnAssembler] User resumed after a pause; \"{}\" will be merged into the next turn",
                     preview(_lastTurnText));
        _carriedText = _lastTurnText;
        _carriedReplacesPrevious = true;
    }
    _lastTurnText.clear();
    _partialAlreadySent = false;
    _latestPartialText.clear();
    _latestPartialConfidence = 0.0f;
    cancelFinalWatchdog();
    keepUnsentTurn();
}

void TurnAssemblerProcessor::keepUnsentTurn() {
    std::string unsent = _pendingTurnEnd ? _accumulatedFinalText : std::string();
    _pendingTurnEnd = false;
    _accumulatedFinalText.clear();
    if (unsent.empty() || _resumeMergeMs == 0) return;
    // The turn detector judged the utterance unfinished and the user proved it right: keep their words.
    spdlog::info("[TurnAssembler] User resumed before \"{}\" was sent; it will be merged into the next turn",
                 preview(unsent));
    _carriedText = _carriedText.empty() ? unsent : _carriedText + " " + unsent;
}

void TurnAssemblerProcessor::handleTranscriptionReady() {
    std::string partial;
    float partialConfidence;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        partial = _latestPartialText;
        partialConfidence = _latestPartialConfidence;
    }
    const int words = countWords(partial);
    const llm::LLMFireContext context{partial, partialConfidence, 1.0f, words};
    if (_firePolicy->evaluate(context) != llm::LLMFireDecision::FIRE_NOW) {
        spdlog::info("[TurnAssembler] {} WAIT_FOR_FINAL on TRANSCRIPTION_READY (partialConf={:.2f}, words={})",
                     _firePolicy->name(), partialConfidence, words);
        return;
    }
    spdlog::info("[TurnAssembler] {} FIRE_NOW on TRANSCRIPTION_READY - partial: \"{}\" (partialConf={:.2f}, words={})",
                 _firePolicy->name(), preview(partial), partialConfidence, words);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _partialAlreadySent = true;
    }
    emitTurn(partial, 1.0f);
}

void TurnAssemblerProcessor::handleUserStoppedSpeaking() {
    std::string staged;
    float stagedConfidence = 0.0f;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pendingTurnEnd && !_accumulatedFinalText.empty()) {
            staged = _accumulatedFinalText;
            stagedConfidence = _accumulatedFinalConfidence;
            _pendingTurnEnd = false;
            _accumulatedFinalText.clear();
        } else {
            spdlog::info("[TurnAssembler] USER_STOPPED_SPEAKING - waiting for the final transcription "
                         "(fallback partial: \"{}\" conf={:.2f})",
                         preview(_latestPartialText), _latestPartialConfidence);
            _pendingTurnEnd = true;
            _pendingFallbackText = _latestPartialText;
            _pendingFallbackConfidence = _latestPartialConfidence;
            _accumulatedFinalText.clear();
            _accumulatedFinalConfidence = 0.0f;
        }
    }
    if (staged.empty()) {
        startFinalWatchdog();
        return;
    }
    cancelFinalWatchdog();
    spdlog::info("[TurnAssembler] USER_STOPPED_SPEAKING - final already staged: \"{}\" (conf={:.2f})",
                 preview(staged), stagedConfidence);
    emitTurn(staged, stagedConfidence);
}

void TurnAssemblerProcessor::handleTurnStateEnd() {
    std::string staged;
    float stagedConfidence = 0.0f;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_pendingTurnEnd || _accumulatedFinalText.empty()) return;
        staged = _accumulatedFinalText;
        stagedConfidence = _accumulatedFinalConfidence;
        _pendingTurnEnd = false;
        _accumulatedFinalText.clear();
    }
    cancelFinalWatchdog();
    spdlog::info("[TurnAssembler] TurnStateFrame END - final already staged: \"{}\" (conf={:.2f})",
                 preview(staged), stagedConfidence);
    emitTurn(staged, stagedConfidence);
}

void TurnAssemblerProcessor::handleFinal(const transport::TranscriptionFrame& frame) {
    const std::string& incoming = frame.transcriptionResult.text;
    const float incomingConfidence = frame.transcriptionResult.confidence;
    std::string text, fallback;
    float confidence = 0.0f, fallbackConfidence = 0.0f;
    bool completesTurn = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pendingTurnEnd) {
            if (!_accumulatedFinalText.empty() && !incoming.empty()) _accumulatedFinalText += " ";
            _accumulatedFinalText += incoming;
            _accumulatedFinalConfidence = std::max(_accumulatedFinalConfidence, incomingConfidence);
            text = _accumulatedFinalText;
            confidence = _accumulatedFinalConfidence;
            fallback = _pendingFallbackText;
            fallbackConfidence = _pendingFallbackConfidence;
            _pendingTurnEnd = false;
            completesTurn = true;
        } else if (_partialAlreadySent) {
            spdlog::info("[TurnAssembler] Dropping the final transcription - the turn was sent on its partial");
            _partialAlreadySent = false;
            return;
        } else if (!incoming.empty()) {
            spdlog::info("[TurnAssembler] Final transcription before the turn end - staged: \"{}\"", preview(incoming));
            _pendingTurnEnd = true;
            _pendingFallbackText = incoming;
            _pendingFallbackConfidence = incomingConfidence;
            _accumulatedFinalText = incoming;
            _accumulatedFinalConfidence = incomingConfidence;
        }
    }
    if (!completesTurn) {
        if (!incoming.empty()) startFinalWatchdog();
        return;
    }
    cancelFinalWatchdog();
    if (!text.empty()) {
        spdlog::info("[TurnAssembler] Final transcription after the turn end: \"{}\" (conf={:.2f})",
                     preview(text), confidence);
        emitTurn(text, confidence);
        return;
    }
    // The final we were waiting for carried no text. Nothing is left to wait for, so fall back to
    // the partial now; waiting for the watchdog loses the turn whenever the user speaks again first.
    emitPartialIfAllowed(fallback, fallbackConfidence, "the final transcription was empty");
}

void TurnAssemblerProcessor::emitPartialIfAllowed(const std::string& text, float confidence, const char* why) {
    if (text.empty()) {
        spdlog::warn("[TurnAssembler] {} and there is no partial to fall back to; no turn", why);
        return;
    }
    const int words = countWords(text);
    if (_firePolicy->evaluate({text, confidence, 0.0f, words}) != llm::LLMFireDecision::FIRE_NOW) {
        spdlog::warn("[TurnAssembler] {}; {} kept the partial back: \"{}\"", why, _firePolicy->name(), preview(text));
        return;
    }
    spdlog::warn("[TurnAssembler] {}; sending the partial: \"{}\" (conf={:.2f}, words={})", why, preview(text),
                 confidence, words);
    emitTurn(text, confidence);
}

void TurnAssemblerProcessor::emitTurn(const std::string& said, float confidence) {
    auto turn = std::make_shared<transport::UserTurnFrame>();
    turn->text = said;
    turn->confidence = confidence;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_carriedText.empty()) {
            turn->text = _carriedText + " " + said;
            turn->replacesPrevious = _carriedReplacesPrevious;
            _carriedText.clear();
            _carriedReplacesPrevious = false;
        }
        _lastTurnText = turn->text;
        _lastTurnAt = std::chrono::steady_clock::now();
        turn->turnId = _nextTurnId++;
    }
    turn->timestamp = transport::Frame::getCurrentTimestamp();
    turn->sourceId = getName();
    if (turn->text.size() != said.size()) {
        spdlog::info("[TurnAssembler] Follow-up merged with the interrupted turn: \"{}\"", preview(turn->text));
        // Tell UIs and transports what is actually answered for this turn.
        auto merged = std::make_shared<transport::TranscriptionFrame>();
        merged->isFinal = true;
        merged->isFollowUp = true;
        merged->transcriptionResult.text = turn->text;
        merged->transcriptionResult.confidence = confidence;
        merged->timestamp = turn->timestamp;
        merged->sourceId = getName();
        pushDownstream(merged);
    }
    pushDownstream(turn);
}

void TurnAssemblerProcessor::startFinalWatchdog() {
    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogArmed = true;
    }
    _watchdogCv.notify_one();
}

void TurnAssemblerProcessor::cancelFinalWatchdog() {
    {
        std::lock_guard<std::mutex> lock(_watchdogMutex);
        _watchdogArmed = false;
    }
    _watchdogCv.notify_one();
}

void TurnAssemblerProcessor::finalWatchdogLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(_watchdogMutex);
        _watchdogCv.wait(lock, [this] { return _watchdogArmed || _watchdogStop; });
        if (_watchdogStop) return;
        const bool timedOut = !_watchdogCv.wait_for(lock, std::chrono::milliseconds(_finalWatchdogTimeoutMs),
                                                    [this] { return !_watchdogArmed || _watchdogStop; });
        if (_watchdogStop) return;
        if (!timedOut || !_watchdogArmed) continue;
        _watchdogArmed = false;
        lock.unlock();

        std::string text;
        float confidence = 0.0f;
        bool isFinalText = false;
        {
            std::lock_guard<std::mutex> turnLock(_mutex);
            if (!_pendingTurnEnd) continue;
            _pendingTurnEnd = false;
            isFinalText = !_accumulatedFinalText.empty();
            text = isFinalText ? _accumulatedFinalText : _pendingFallbackText;
            confidence = isFinalText ? _accumulatedFinalConfidence : _pendingFallbackConfidence;
            _accumulatedFinalText.clear();
        }
        if (isFinalText) {
            spdlog::info("[TurnAssembler] Watchdog timeout - sending the staged final: \"{}\" (conf={:.2f})",
                         preview(text), confidence);
            emitTurn(text, confidence);
        } else {
            emitPartialIfAllowed(text, confidence, "Watchdog timeout - no final transcription");
        }
    }
}

} // namespace pipeline
} // namespace va
