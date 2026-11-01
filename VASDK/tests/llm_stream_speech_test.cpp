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
//     the final differs, or when the speculative reply wants a tool.

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
    bool anthropic = false;
    std::string lastBody;
    std::atomic<bool> cancelled{false};
    Clock::time_point cancelTime, returnTime;

    network::HttpResponse post(const std::string&, const std::map<std::string, std::string>&, const std::string& body,
                               int, std::function<void(const std::string&)> onChunk) override {
        cancelled = false;
        lastBody = body;
        const int n = requests++;
        const std::string reply =
            replies.empty() ? this->reply : replies[std::min<size_t>(n, replies.size() - 1)];
        network::HttpResponse r;
        for (size_t i = 0; i < reply.size(); i += 5) {
            if (cancelled) {
                r.error = {network::NetworkErrorCode::Cancelled, "cancelled"};
                returnTime = Clock::now();
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
        return r;
    }
    network::HttpResponse get(const std::string&, const std::map<std::string, std::string>&, int) override { return {}; }
    bool sendText(const std::string&, const std::map<std::string, std::string>&, const std::string&, int) override { return false; }
    bool sendBytes(const std::string&, const std::map<std::string, std::string>&, const std::vector<uint8_t>&, int) override { return false; }
    void cancel() override {
        cancelTime = Clock::now();
        cancelled = true;
    }
};

struct Spoken {
    std::string text;
    bool isFinal, isExpecting;
    Clock::time_point at;
};

class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> f, pipeline::FlowDirection) override {
        if (f->getType() != transport::FrameType::TEXT) return;
        auto t = std::static_pointer_cast<transport::TextFrame>(f);
        std::lock_guard<std::mutex> l(m);
        frames.push_back({t->text, t->isFinal, t->isExpecting, Clock::now()});
    }
    std::vector<Spoken> snapshot() {
        std::lock_guard<std::mutex> l(m);
        return frames;
    }
    std::mutex m;
    std::vector<Spoken> frames;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

struct Rig {
    std::shared_ptr<FakeHttp> http = std::make_shared<FakeHttp>();
    std::shared_ptr<llm::LLMTransport> transport;
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
    }
    ~Rig() {
        llm->stop();
        if (tools) tools->stop();
        recorder->stop();
    }

    void send(std::shared_ptr<transport::Frame> f) { llm->processFrame(f, pipeline::FlowDirection::DOWNSTREAM); }

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
        llm->processFrame(stopped, pipeline::FlowDirection::DOWNSTREAM);
        auto final = std::make_shared<transport::TranscriptionFrame>();
        final->isFinal = true;
        final->transcriptionResult.text = text;
        final->transcriptionResult.confidence = 1.0f;
        llm->processFrame(final, pipeline::FlowDirection::DOWNSTREAM);
    }
};

void streamsSentencesThenTail(bool anthropic) {
    Rig rig(R"({"text": "Sure, I can help with that. Your order shipped yesterday. Anything else?", "isExpecting": true})",
            anthropic);
    rig.userSays("Where is my order?");
    std::this_thread::sleep_for(1500ms);

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
    for (int i = 0; i < 100 && rig.recorder->snapshot().empty(); ++i) std::this_thread::sleep_for(10ms);

    auto interrupt = std::make_shared<transport::AgentStateFrame>();
    interrupt->transition = transport::AgentStateTransition::BARGE_IN_INTERRUPT;
    rig.llm->processFrame(interrupt, pipeline::FlowDirection::DOWNSTREAM);
    const size_t spokenAtInterrupt = rig.recorder->snapshot().size();
    std::this_thread::sleep_for(1500ms);

    check(spokenAtInterrupt == 1, "first sentence was spoken before the barge-in");
    check(rig.http->cancelled, "HTTP request was cancelled");
    check(rig.http->returnTime - rig.http->cancelTime < 100ms, "request aborted promptly");
    check(rig.recorder->snapshot().size() == spokenAtInterrupt, "nothing spoken after the barge-in");
    bool userTurnKept = false;
    for (const auto& m : rig.transport->getHistory())
        if (m.role == llm::MessageRole::USER || m.role == llm::MessageRole::ASSISTANT) userTurnKept = true;
    check(!userTurnKept, "aborted turn rolled back from history");
}

void toolCallSpeaksNothing() {
    Rig rig(R"({"tools": [{"tool": "lookup_order", "args": {"id": "42"}}]})");
    rig.userSays("Where is order 42?");
    std::this_thread::sleep_for(1200ms);
    check(rig.recorder->snapshot().empty(), "tool-call reply speaks nothing");
}

void toolRoundTripWithFiller() {
    Rig rig("", false, true);
    rig.http->replies = {R"({"tools": [{"tool": "lookup_order", "args": {}}]})",
                         R"({"text": "Your order has shipped. It arrives on Friday."})"};
    rig.userSays("Where is my order?");
    std::this_thread::sleep_for(2500ms);

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
    std::this_thread::sleep_for(1000ms);  // the speculative reply has fully streamed by now
    check(rig.recorder->snapshot().empty(), "[spec confirmed] nothing spoken before the turn is confirmed");

    const auto confirmAt = Clock::now();
    rig.userSays("Where is my order?");
    std::this_thread::sleep_for(100ms);
    const auto frames = rig.recorder->snapshot();
    check(rig.http->requests == 1, "[spec confirmed] no second request");
    check(frames.size() == 2 && frames[0].text == "Your order shipped yesterday. " && frames[1].isFinal,
          "[spec confirmed] held reply released");
    if (!frames.empty()) check(frames[0].at - confirmAt < 50ms, "[spec confirmed] released immediately");
    check(rig.userTurnsInHistory() == std::vector<std::string>{"Where is my order"}, "[spec confirmed] history kept");
}

void speculationReplacedWhenFinalDiffers() {
    Rig rig(kReply);
    rig.llm->setSpeculativeFiring(true, 3);
    rig.userPausesAfter("Where is my");
    std::this_thread::sleep_for(400ms);
    rig.userSays("Where is my order?");
    std::this_thread::sleep_for(1500ms);
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
    std::this_thread::sleep_for(1200ms);
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
    std::this_thread::sleep_for(800ms);
    check(toolRuns == 0, "[spec tools] tool not run on an unconfirmed turn");
    rig.userSays("Where is my order?");
    std::this_thread::sleep_for(2500ms);
    check(toolRuns == 1, "[spec tools] tool runs once after confirmation");
    check(rig.http->requests == 3, "[spec tools] fresh request, then the tool follow-up");
}

} // namespace

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
    std::printf("llm stream speech scenarios done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
