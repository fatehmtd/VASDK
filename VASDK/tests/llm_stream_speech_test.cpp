// LLMProcessor stream-to-speech, end to end through the real OpenAITransport (SSE parsing) over a
// fake HTTP client that streams a scripted reply in small deltas:
//   - sentences go downstream as isFinal=false frames before the stream ends, then one final frame
//     carries only the unspoken tail plus isExpecting;
//   - a barge-in mid-stream aborts the request promptly, nothing more is spoken, and the aborted
//     user turn is rolled back from history;
//   - a tool-call reply speaks nothing;
//   - the same streaming works through AnthropicTransport's SSE events, which send the user turn once;
//   - a tool call speaks the filler phrase at once and the turn stays open through the tool round
//     trip until the follow-up reply's final frame;
//   - speculative firing: the request starts on the partial at a VAD pause, its speech is held until
//     the final transcript confirms it, and is cancelled + rolled back when the user resumes, when
//     the final differs, or when the speculative reply wants a tool;
//   - a follow-up: the user resumes while the agent is thinking, so the interrupted turn and the new
//     one reach the LLM as a single message.
// Frames go through a TurnAssemblerProcessor first, as in a real pipeline: it decides the turns.

#include <VASDK/TurnAssemblerProcessor.h>
#include <VASDK/llm/LLMProcessor.h>
#include <VASDK/llm/AnthropicTransport.h>
#include <VASDK/llm/OpenAITransport.h>
#include <VASDK/llm/ToolDispatchProcessor.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// Streams `reply` as OpenAI (or Anthropic) SSE deltas of 5 characters, 30 ms apart; honours cancel().
class FakeHttp : public network::HttpInterface {
public:
    std::string reply;
    std::vector<std::string> replies;  // one per request when set (tool round trips)
    std::atomic<int> requests{0};
    std::atomic<int> finished{0};   // requests that have returned: done, cancelled or failed
    bool anthropic = false;
    bool timesOut = false;   // every request fails, as a timeout does
    std::string lastBody, lastUrl;
    std::atomic<bool> cancelled{false};
    std::atomic<int> generation{0};  // like CurlHttpProvider: a cancel hits requests started before it
    Clock::time_point cancelTime, returnTime;

    network::HttpResponse post(const std::string& url, const std::map<std::string, std::string>&, const std::string& body,
                               int, std::function<void(const std::string&)> onChunk) override {
        cancelled = false;
        const int gen = generation.load();
        lastBody = body;
        lastUrl = url;
        if (timesOut) {
            ++requests;
            network::HttpResponse failed;
            failed.error = {network::NetworkErrorCode::Timeout, "Timeout was reached"};
            ++finished;
            return failed;
        }
        const int n = requests++;
        const std::string reply =
            replies.empty() ? this->reply : replies[std::min<size_t>(n, replies.size() - 1)];
        network::HttpResponse r;
        for (size_t i = 0; i < reply.size(); i += 5) {
            if (generation.load() != gen) {
                r.error = {network::NetworkErrorCode::Cancelled, "cancelled"};
                returnTime = Clock::now();
                ++finished;
                return r;
            }
            const std::string piece = jsonEscape(reply.substr(i, 5));
            const std::string line =
                anthropic ? "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
                            "\"delta\":{\"type\":\"text_delta\",\"text\":\"" + piece + "\"}}\n\n"
                          : "data: {\"choices\":[{\"delta\":{\"content\":\"" + piece + "\"}}]}\n\n";
            r.body += line;
            if (onChunk) onChunk(line);
            std::this_thread::sleep_for(30ms);
        }
        if (onChunk) onChunk(anthropic ? "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n" : "data: [DONE]\n\n");
        r.statusCode = 200;
        returnTime = Clock::now();
        ++finished;
        return r;
    }
    network::HttpResponse get(const std::string&, const std::map<std::string, std::string>&, int) override { return {}; }
    bool sendText(const std::string&, const std::map<std::string, std::string>&, const std::string&, int) override { return false; }
    bool sendBytes(const std::string&, const std::map<std::string, std::string>&, const std::vector<uint8_t>&, int) override { return false; }
    void cancel() override {
        cancelTime = Clock::now();
        cancelled = true;
        ++generation;
    }
};

struct Spoken {
    std::string text;
    bool isFinal, isExpecting;
    Clock::time_point at;
    uint64_t turnId;
};

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> f, pipeline::FlowDirection) override {
        if (auto t = std::dynamic_pointer_cast<transport::TranscriptionFrame>(f); t && t->isFollowUp) {
            std::lock_guard<std::mutex> l(m);
            followUps.push_back(t->transcriptionResult.text);
        }
        if (f->getType() != transport::FrameType::TEXT) return;
        auto t = std::static_pointer_cast<transport::TextFrame>(f);
        std::lock_guard<std::mutex> l(m);
        frames.push_back({t->text, t->isFinal, t->isExpecting, Clock::now(), t->turnId});
    }
    std::vector<Spoken> snapshot() {
        std::lock_guard<std::mutex> l(m);
        return frames;
    }
    std::mutex m;
    std::vector<Spoken> frames;
    std::vector<std::string> followUps;  // merged text announced downstream
};

// Until `condition` holds, at most `timeout`. A check that something happens waits for it, not a
// fixed time: a slow machine (a shared CI runner) takes longer than the fake's 30 ms steps add up
// to. A check that something doesn't happen still waits a fixed time.
template <class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = Clock::now() + timeout;
    while (!condition()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

size_t finalFrames(Recorder& recorder) {
    size_t count = 0;
    for (const auto& spoken : recorder.snapshot()) count += spoken.isFinal ? 1 : 0;
    return count;
}

// Until `count` replies have said their final frame
void waitForFinalFrames(Recorder& recorder, size_t count = 1) {
    waitUntil([&] { return finalFrames(recorder) >= count; });
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

struct Rig {
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::shared_ptr<llm::LLMTransport> transport;
    std::shared_ptr<pipeline::TurnAssemblerProcessor> assembler = std::make_shared<pipeline::TurnAssemblerProcessor>();
    std::shared_ptr<llm::LLMProcessor> llm;
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();

    std::shared_ptr<llm::ToolDispatchProcessor> tools;

    explicit Rig(const std::string& reply, bool anthropic = false, bool withTools = false) {
        http->reply = reply;
        http->anthropic = anthropic;
        llm::LLMConfig cfg;
        cfg.apiKey    = "test";
        cfg.endpoint  = "http://localhost:1/v1/chat/completions";
        cfg.model     = "fake";
        cfg.streaming = true;
        if (anthropic) transport = std::make_shared<llm::AnthropicTransport>(cfg, http);
        else           transport = std::make_shared<llm::OpenAITransport>(cfg, http);
        llm = std::make_shared<llm::LLMProcessor>(transport);
        llm->setStreamSpeech(true);
        if (withTools) {
            tools = std::make_shared<llm::ToolDispatchProcessor>();
            tools->registerTool(llm::Tool("lookup_order", "Look up an order")
                                    .onCall([](const nlohmann::json&) { return nlohmann::json{{"status", "shipped"}}; }));
            llm->setToolFillerPhrases({"One moment."});
            llm->setToolDispatcher(tools);
            llm->setDownstream(tools);
            tools->setDownstream(recorder);
            tools->initialize();
            tools->start();
        } else {
            llm->setDownstream(recorder);
        }
        recorder->initialize();
        recorder->start();
        llm->initialize();
        llm->start();
        assembler->setDownstream(llm);
        assembler->initialize();
        assembler->start();
    }
    ~Rig() {
        assembler->stop();
        llm->stop();
        if (tools) tools->stop();
        recorder->stop();
    }

    void send(std::shared_ptr<transport::Frame> f) { assembler->processFrame(f, pipeline::FlowDirection::DOWNSTREAM); }

    // Agent listening, streaming STT partial cached, then the VAD reports a sustained pause.
    void userPausesAfter(const std::string& partialText) {
        auto listening = std::make_shared<transport::AgentStateFrame>();
        listening->currentState = transport::AgentState::LISTENING;
        listening->transition   = transport::AgentStateTransition::PIPELINE_START;
        send(listening);
        auto partial = std::make_shared<transport::TranscriptionFrame>();
        partial->isFinal = false;
        partial->transcriptionResult.text = partialText;
        partial->transcriptionResult.confidence = 0.9f;
        send(partial);
        vad(transport::VADState::ENDING);
    }
    void vad(transport::VADState state) {
        auto f = std::make_shared<transport::VADStateFrame>();
        f->state = state;
        send(f);
    }
    std::vector<std::string> userTurnsInHistory() const {
        std::vector<std::string> out;
        for (const auto& m : transport->getHistory())
            if (m.role == llm::MessageRole::USER) out.push_back(m.content);
        return out;
    }

    void userSays(const std::string& text) {
        auto stopped = std::make_shared<transport::AgentStateFrame>();
        stopped->transition = transport::AgentStateTransition::USER_STOPPED_SPEAKING;
        send(stopped);
        auto final = std::make_shared<transport::TranscriptionFrame>();
        final->isFinal = true;
        final->transcriptionResult.text = text;
        final->transcriptionResult.confidence = 1.0f;
        send(final);
    }
};

void streamsSentencesThenTail(bool anthropic) {
    Rig rig(R"({"text": "Sure, I can help with that. Your order shipped yesterday. Anything else?", "isExpecting": true})",
            anthropic);
    rig.userSays("Where is my order?");
    waitForFinalFrames(*rig.recorder);

    size_t userTurns = 0;
    for (size_t at = 0; (at = rig.http->lastBody.find("Where is my order?", at)) != std::string::npos; ++at) ++userTurns;
    check(userTurns == 1, "request carries the user turn exactly once");

    const auto frames = rig.recorder->snapshot();
    check(frames.size() == 3, "two sentence frames plus one final frame");
    if (frames.size() == 3) {
        check(frames[0].text == "Sure, I can help with that. " && !frames[0].isFinal, "first sentence streamed");
        check(frames[1].text == "Your order shipped yesterday. " && !frames[1].isFinal, "second sentence streamed");
        check(frames[2].text == "Anything else?" && frames[2].isFinal && frames[2].isExpecting,
              "final frame carries only the tail and isExpecting");
        check(frames[0].at < rig.http->returnTime - 300ms, "first sentence spoken well before the stream ended");
    }
}

void bargeInAbortsAndRollsBack() {
    Rig rig(R"({"text": "Let me explain all the options in detail. First, there is the basic plan. Then the premium plan. Finally the family plan."})");
    rig.userSays("What plans do you have?");
    waitUntil([&] { return !rig.recorder->snapshot().empty(); });

    auto interrupt = std::make_shared<transport::AgentStateFrame>();
    interrupt->transition = transport::AgentStateTransition::BARGE_IN_INTERRUPT;
    rig.send(interrupt);
    const size_t spokenAtInterrupt = rig.recorder->snapshot().size();
    waitUntil([&] { return rig.http->finished == 1; });
    std::this_thread::sleep_for(200ms);   // nothing more is spoken

    check(spokenAtInterrupt == 1, "first sentence was spoken before the barge-in");
    check(rig.http->cancelled, "HTTP request was cancelled");
    // Promptly: the rest of the reply would have streamed for over half a second more
    check(rig.http->returnTime - rig.http->cancelTime < 300ms, "request aborted promptly");
    check(rig.recorder->snapshot().size() == spokenAtInterrupt, "nothing spoken after the barge-in");
    bool userTurnKept = false;
    for (const auto& m : rig.transport->getHistory())
        if (m.role == llm::MessageRole::USER || m.role == llm::MessageRole::ASSISTANT) userTurnKept = true;
    check(!userTurnKept, "aborted turn rolled back from history");
}

void toolCallSpeaksNothing() {
    Rig rig(R"({"tools": [{"tool": "lookup_order", "args": {"id": "42"}}]})");
    rig.userSays("Where is order 42?");
    waitUntil([&] { return rig.http->finished == 1; });
    std::this_thread::sleep_for(200ms);
    check(rig.recorder->snapshot().empty(), "tool-call reply speaks nothing");
}

void toolRoundTripWithFiller() {
    Rig rig("", false, true);
    rig.http->replies = {R"({"tools": [{"tool": "lookup_order", "args": {}}]})",
                         R"({"text": "Your order has shipped. It arrives on Friday."})"};
    rig.userSays("Where is my order?");
    waitForFinalFrames(*rig.recorder);

    const auto frames = rig.recorder->snapshot();
    check(rig.http->requests == 2, "tool round trip made a follow-up request");
    check(frames.size() == 3, "filler, one follow-up sentence, one final frame");
    if (frames.size() == 3) {
        check(frames[0].text == "One moment. " && !frames[0].isFinal, "filler spoken first, turn left open");
        check(frames[1].text == "Your order has shipped. " && !frames[1].isFinal, "follow-up sentence streamed");
        check(frames[2].text == "It arrives on Friday." && frames[2].isFinal, "follow-up final closes the turn");
    }
}

const char* kReply = R"({"text": "Your order shipped yesterday. It arrives on Friday."})";

void speculationConfirmed() {
    Rig rig(kReply);
    rig.llm->setSpeculativeFiring(true, 3);
    rig.userPausesAfter("Where is my order");
    waitUntil([&] { return rig.http->finished == 1; });   // the speculative reply has fully streamed
    std::this_thread::sleep_for(100ms);
    check(rig.recorder->snapshot().empty(), "[spec confirmed] nothing spoken before the turn is confirmed");

    const auto confirmAt = Clock::now();
    rig.userSays("Where is my order?");
    waitForFinalFrames(*rig.recorder);
    const auto frames = rig.recorder->snapshot();
    check(rig.http->requests == 1, "[spec confirmed] no second request");
    check(frames.size() == 2 && frames[0].text == "Your order shipped yesterday. " && frames[1].isFinal,
          "[spec confirmed] held reply released");
    // Immediately: a fresh request would stream for over half a second first
    if (!frames.empty()) check(frames[0].at - confirmAt < 300ms, "[spec confirmed] released immediately");
    check(rig.userTurnsInHistory() == std::vector<std::string>{"Where is my order"}, "[spec confirmed] history kept");
}

void speculationReplacedWhenFinalDiffers() {
    Rig rig(kReply);
    rig.llm->setSpeculativeFiring(true, 3);
    rig.userPausesAfter("Where is my");
    std::this_thread::sleep_for(400ms);
    rig.userSays("Where is my order?");
    waitForFinalFrames(*rig.recorder);
    check(rig.http->requests == 2, "[spec differs] fresh request for the final transcript");
    check(rig.recorder->snapshot().size() == 2, "[spec differs] only the fresh reply is spoken");
    check(rig.userTurnsInHistory() == std::vector<std::string>{"Where is my order?"},
          "[spec differs] speculative turn rolled back");
}

void speculationCancelledWhenUserResumes() {
    Rig rig(kReply);
    rig.llm->setSpeculativeFiring(true, 3);
    rig.userPausesAfter("Where is my order");
    std::this_thread::sleep_for(200ms);
    rig.vad(transport::VADState::RESUMING);
    waitUntil([&] { return rig.http->finished == 1; });
    std::this_thread::sleep_for(200ms);
    check(rig.recorder->snapshot().empty(), "[spec resumed] nothing spoken");
    check(rig.http->cancelled, "[spec resumed] request cancelled");
    check(rig.userTurnsInHistory().empty(), "[spec resumed] speculative turn rolled back");
}

void speculativeToolCallNeverRuns() {
    Rig rig("", false, true);
    std::atomic<int> toolRuns{0};
    rig.tools->registerTool(llm::Tool("lookup_order", "Look up an order").onCall([&](const nlohmann::json&) {
        ++toolRuns;
        return nlohmann::json{{"status", "shipped"}};
    }));
    rig.http->replies = {R"({"tools": [{"tool": "lookup_order", "args": {}}]})",
                         R"({"tools": [{"tool": "lookup_order", "args": {}}]})",
                         R"({"text": "Your order has shipped. It arrives on Friday."})"};
    rig.llm->setSpeculativeFiring(true, 3);
    rig.userPausesAfter("Where is my order");
    waitUntil([&] { return rig.http->finished == 1; });
    std::this_thread::sleep_for(100ms);
    check(toolRuns == 0, "[spec tools] tool not run on an unconfirmed turn");
    rig.userSays("Where is my order?");
    waitUntil([&] { return toolRuns == 1 && rig.http->requests == 3 && rig.http->finished == 3; });
    check(toolRuns == 1, "[spec tools] tool runs once after confirmation");
    check(rig.http->requests == 3, "[spec tools] fresh request, then the tool follow-up");
}

void extraPayloadMergedIntoRequests() {
    llm::LLMConfig cfg;
    cfg.apiKey = "test";
    cfg.endpoint = "http://localhost:1/v1/chat/completions";
    cfg.model = "fake";
    cfg.extraPayload = R"({"provider": {"sort": "latency"}, "max_tokens": 64})";
    for (bool anthropic : {false, true}) {
        std::shared_ptr<llm::LLMTransport> t;
        if (anthropic) t = std::make_shared<llm::AnthropicTransport>(cfg, std::make_shared<FakeHttp>());
        else           t = std::make_shared<llm::OpenAITransport>(cfg, std::make_shared<FakeHttp>());
        const auto body = nlohmann::json::parse(t->previewRequestPayload());
        check(body.value("/provider/sort"_json_pointer, std::string()) == "latency" && body["max_tokens"] == 64 &&
                  body["model"] == "fake",
              anthropic ? "[extraPayload] merged into Anthropic requests" : "[extraPayload] merged into OpenAI requests");
    }
}

// A turn that names an LLM profile (a route's llm_profile) is answered by it; the next turn that
// names none by the default. One conversation, one history, whichever model answers.
void profileAnswersTheTurnsThatNameIt() {
    Rig rig(R"({"text": "Sure."})");
    llm::LLMConfig smart = rig.transport->getConfig();
    smart.model = "smart-model";
    smart.endpoint = "http://localhost:2/v1/chat/completions";
    rig.llm->setProfiles({{"smart", smart}});
    auto turn = [&rig](const std::string& text, const std::string& profile) {
        auto frame = std::make_shared<transport::UserTurnFrame>();
        frame->text = text;
        frame->confidence = 1.0f;
        frame->llmProfile = profile;
        const int before = rig.http->finished;
        rig.llm->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
        waitUntil([&] { return rig.http->finished > before; });
        return nlohmann::json::parse(rig.http->lastBody).value("model", std::string());
    };
    check(turn("Tell me a story.", "smart") == "smart-model" && rig.http->lastUrl == smart.endpoint,
          "[profiles] the turn that names one is answered by it, at its endpoint");
    check(turn("Thanks.", "") == "fake" && rig.http->lastUrl == "http://localhost:1/v1/chat/completions",
          "[profiles] the next turn by the default");
    check(turn("Again.", "nope") == "fake", "[profiles] an unknown one: the default");
    waitUntil([&] { return finalFrames(*rig.recorder) == 3; });
    check(rig.userTurnsInHistory().size() == 3, "[profiles] one history across them");
}

// The stale turn reaches the LLM after the interrupt (a slow Dialog), the merged one right behind
// it, as TurnAssemblerProcessor sends them: only the merged one is answered, its frames tagged with it.
void replacingTurnCancelsThePreviousReply() {
    Rig rig("");
    rig.http->replies = {R"({"text": "Got it. What would you like to do next?"})",
                         R"({"text": "Once upon a time there was a stone."})"};
    auto turn = [&rig](const std::string& text, uint64_t turnId, bool replacesPrevious) {
        if (replacesPrevious) {
            auto replaced = std::make_shared<transport::TurnReplacedFrame>();
            replaced->turnId = turnId - 1;
            rig.llm->processFrame(replaced, pipeline::FlowDirection::DOWNSTREAM);
        }
        auto frame = std::make_shared<transport::UserTurnFrame>();
        frame->text = text;
        frame->confidence = 1.0f;
        frame->turnId = turnId;
        frame->replacesPrevious = replacesPrevious;
        rig.llm->processFrame(frame, pipeline::FlowDirection::DOWNSTREAM);
    };
    turn("Actually.", 3, false);
    std::this_thread::sleep_for(50ms);   // its request is streaming
    turn("Actually. Tell me a story.", 4, true);
    waitForFinalFrames(*rig.recorder);
    std::string spoken;
    bool tagged = true;
    for (const auto& frame : rig.recorder->snapshot()) {
        spoken += frame.text;
        tagged = tagged && frame.turnId == 4;
    }
    check(spoken == "Once upon a time there was a stone.", "[replaces] only the merged turn's reply is said");
    check(tagged, "[replaces] its frames carry the turn they answer");
    check(rig.userTurnsInHistory() == std::vector<std::string>{"Actually. Tell me a story."},
          "[replaces] only the merged turn in history");

    // A turn the LLM never got (the Dialog answered it): the exchange before it stays.
    turn("Thanks.", 5, false);
    waitForFinalFrames(*rig.recorder, 2);
    auto replaced = std::make_shared<transport::TurnReplacedFrame>();
    replaced->turnId = 6;
    rig.llm->processFrame(replaced, pipeline::FlowDirection::DOWNSTREAM);
    std::this_thread::sleep_for(100ms);
    check(rig.userTurnsInHistory().size() == 2, "[replaces] another turn's exchange isn't rolled back");
}

// A request that fails with nothing said: the user hears that, not silence.
void failedRequestIsSaid() {
    Rig rig(R"({"text": "Sure."})");
    rig.llm->setFailureReplies({"Sorry, that took too long."});
    rig.http->timesOut = true;
    rig.userSays("Tell me a long story.");
    waitForFinalFrames(*rig.recorder);
    const auto frames = rig.recorder->snapshot();
    check(!frames.empty() && frames.back().text == "Sorry, that took too long." && frames.back().isFinal,
          "[failure] a failed request is said, not left in silence");
}

void backchannelGoesWithTheNextMessage() {
    Rig rig(R"({"text": "Sure."})");
    auto backchannel = std::make_shared<transport::TranscriptionFrame>();
    backchannel->isFinal = true;
    backchannel->isBackchannel = true;
    backchannel->transcriptionResult.text = "mm-hmm";
    rig.send(backchannel);
    std::this_thread::sleep_for(100ms);
    check(rig.http->requests == 0, "[backchannel] not answered");
    rig.userSays("What about Tuesday?");
    waitForFinalFrames(*rig.recorder);
    const auto userTurns = rig.userTurnsInHistory();
    check(userTurns.size() == 1 && userTurns[0] == "(While you were speaking I said \"mm-hmm\".) What about Tuesday?",
          "[backchannel] recorded at the front of the next user message");
    rig.userSays("And Wednesday?");
    waitForFinalFrames(*rig.recorder, 2);
    check(rig.userTurnsInHistory().size() == 2 && rig.userTurnsInHistory()[1] == "And Wednesday?",
          "[backchannel] recorded once");
}

// The agent's replies pass the LLM on their way to TTS: each goes into its history with the words
// it answered, so a later turn the LLM answers knows what the agent did; private values are masked.
void agentRepliesGoInHistory() {
    Rig rig(R"({"text": "Yes, I did."})");
    auto part = [](const std::string& text, bool isFinal, const std::string& answers, std::vector<std::string> sensitive = {}) {
        auto frame = std::make_shared<transport::TextFrame>();
        frame->text = text;
        frame->isFinal = isFinal;
        frame->answers = answers;
        frame->sensitiveValues = std::move(sensitive);
        return frame;
    };
    rig.send(part("Hi! What can I do for you?", true, ""));                    // a greeting: no user words
    rig.send(part("Calling John.", false, ""));
    rig.send(part(" What can I do for you?", true, "call john"));
    rig.send(part("Your PIN 4321 is set.", true, "", {"4321"}));
    std::this_thread::sleep_for(100ms);
    check(rig.http->requests == 0, "[agent] its replies aren't answered");
    const size_t agentFinals = finalFrames(*rig.recorder);
    rig.userSays("Did you call John?");
    waitForFinalFrames(*rig.recorder, agentFinals + 1);
    std::vector<std::pair<llm::MessageRole, std::string>> said;
    for (const auto& m : rig.transport->getHistory())
        if (m.role != llm::MessageRole::SYSTEM) said.emplace_back(m.role, m.content);
    using R = llm::MessageRole;
    const std::vector<std::pair<llm::MessageRole, std::string>> expected = {
        {R::ASSISTANT, "Hi! What can I do for you?"}, {R::USER, "call john"}, {R::ASSISTANT, "Calling John. What can I do for you?"},
        {R::ASSISTANT, "Your PIN [private] is set."}, {R::USER, "Did you call John?"}, {R::ASSISTANT, R"({"text": "Yes, I did."})"}};   // the LLM's own, as it wrote it
    check(said == expected, "[agent] its turns are in the history, in order, the PIN masked");
    if (said != expected)
        for (const auto& [role, content] : said) std::fprintf(stderr, "  %d: %s\n", static_cast<int>(role), content.c_str());
}

} // namespace

void followUpMergedWithInterruptedTurn() {
    for (const bool enabled : {true, false}) {
        Rig rig(kReply);
        if (!enabled) rig.assembler->setResumeMergeMs(0);
        rig.userSays("Book a flight");
        std::this_thread::sleep_for(100ms);  // request in flight, nothing spoken yet
        auto resumed = std::make_shared<transport::AgentStateFrame>();
        resumed->transition = transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING;
        rig.send(resumed);
        rig.userSays("to Paris");
        const std::vector<std::string> expected{enabled ? "Book a flight to Paris" : "to Paris"};
        waitUntil([&] { return rig.userTurnsInHistory() == expected && finalFrames(*rig.recorder) >= 1; });
        check(rig.userTurnsInHistory() == expected,
              enabled ? "[follow-up] both utterances sent as one turn" : "[follow-up off] only the new utterance sent");
        check(rig.recorder->followUps == (enabled ? expected : std::vector<std::string>{}),
              "[follow-up] merged text announced downstream only when merging");
    }
}

void followUpKeepsStagedUnsentFinal() {
    // The turn detector judged "...a flight to" unfinished: its final is staged, never sent.
    Rig rig(kReply);
    auto staged = std::make_shared<transport::TranscriptionFrame>();
    staged->isFinal = true;
    staged->transcriptionResult.text = "Book a flight to";
    staged->transcriptionResult.confidence = 1.0f;
    rig.send(staged);
    auto resumed = std::make_shared<transport::AgentStateFrame>();
    resumed->transition = transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING;
    rig.send(resumed);
    rig.userSays("Paris.");
    waitForFinalFrames(*rig.recorder);
    check(rig.http->requests == 1, "[follow-up staged] only the merged turn is requested");
    check(rig.userTurnsInHistory() == std::vector<std::string>{"Book a flight to Paris."},
          "[follow-up staged] unsent words merged into the next turn");
}

int main() {
    streamsSentencesThenTail(false);
    streamsSentencesThenTail(true);
    bargeInAbortsAndRollsBack();
    toolCallSpeaksNothing();
    toolRoundTripWithFiller();
    speculationConfirmed();
    speculationReplacedWhenFinalDiffers();
    speculationCancelledWhenUserResumes();
    speculativeToolCallNeverRuns();
    extraPayloadMergedIntoRequests();
    profileAnswersTheTurnsThatNameIt();
    replacingTurnCancelsThePreviousReply();
    failedRequestIsSaid();
    backchannelGoesWithTheNextMessage();
    agentRepliesGoInHistory();
    followUpMergedWithInterruptedTurn();
    followUpKeepsStagedUnsentFinal();
    std::printf("llm stream speech scenarios done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
