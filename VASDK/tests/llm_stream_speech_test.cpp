// LLMProcessor stream-to-speech, end to end through the real OpenAITransport (SSE parsing) over a
// fake HTTP client that streams a scripted reply in small deltas:
//   - sentences go downstream as isFinal=false frames before the stream ends, then one final frame
//     carries only the unspoken tail plus isExpecting;
//   - a barge-in mid-stream aborts the request promptly, nothing more is spoken, and the aborted
//     user turn is rolled back from history;
//   - a tool-call reply speaks nothing.

#include <VASDK/llm/LLMProcessor.h>
#include <VASDK/llm/OpenAITransport.h>

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

// Streams `reply` as OpenAI SSE deltas of 5 characters, 30 ms apart; honours cancel().
class FakeHttp : public network::HttpInterface {
public:
    std::string reply;
    std::atomic<bool> cancelled{false};
    Clock::time_point cancelTime, returnTime;

    network::HttpResponse post(const std::string&, const std::map<std::string, std::string>&, const std::string&,
                               int, std::function<void(const std::string&)> onChunk) override {
        cancelled = false;
        network::HttpResponse r;
        for (size_t i = 0; i < reply.size(); i += 5) {
            if (cancelled) {
                r.error = {network::NetworkErrorCode::Cancelled, "cancelled"};
                returnTime = Clock::now();
                return r;
            }
            const std::string line =
                "data: {\"choices\":[{\"delta\":{\"content\":\"" + jsonEscape(reply.substr(i, 5)) + "\"}}]}\n\n";
            r.body += line;
            if (onChunk) onChunk(line);
            std::this_thread::sleep_for(30ms);
        }
        if (onChunk) onChunk("data: [DONE]\n\n");
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
    std::shared_ptr<llm::OpenAITransport> transport;
    std::shared_ptr<llm::LLMProcessor> llm;
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>();

    explicit Rig(const std::string& reply) {
        http->reply = reply;
        llm::LLMConfig cfg;
        cfg.apiKey    = "test";
        cfg.endpoint  = "http://localhost:1/v1/chat/completions";
        cfg.model     = "fake";
        cfg.streaming = true;
        transport = std::make_shared<llm::OpenAITransport>(cfg, http);
        llm = std::make_shared<llm::LLMProcessor>(transport);
        llm->setStreamSpeech(true);
        llm->setDownstream(recorder);
        recorder->initialize();
        recorder->start();
        llm->initialize();
        llm->start();
    }
    ~Rig() { llm->stop(); recorder->stop(); }

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

void streamsSentencesThenTail() {
    Rig rig(R"({"text": "Sure, I can help with that. Your order shipped yesterday. Anything else?", "isExpecting": true})");
    rig.userSays("Where is my order?");
    std::this_thread::sleep_for(1500ms);

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

} // namespace

int main() {
    streamsSentencesThenTail();
    bargeInAbortsAndRollsBack();
    toolCallSpeaksNothing();
    std::printf("llm stream speech scenarios done, %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
