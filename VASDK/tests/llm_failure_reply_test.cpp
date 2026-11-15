// LLMProcessor's failure reply ("Sorry, I couldn't get an answer just now") is said only when the
// LLM gave no answer. A transport that delivers its reply itself, as frames through
// fireOnFrameReceived, returns "" from sendLLMRequest (as LlamaCppTransport does): its answer is
// said once, with no failure reply after it, and so is the reply after a native tool call. A
// transport that delivers nothing gets the failure reply.

#include <VASDK/llm/LLMProcessor.h>
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/llm/ToolDispatchProcessor.h>

#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace va;
using namespace std::chrono_literals;
using va::test::waitUntil;

namespace {

// Answers every request with `reply`, delivered as one final frame of its own, and returns "" as
// LlamaCppTransport does; an empty `reply` is a request that failed. With `firstCallsTool`, the
// first request calls that tool instead, as a native ToolCallFrame (llama.cpp's LFM2 tool tokens),
// and the follow-up after its result is answered with `reply`.
class SelfDeliveringTransport : public llm::LLMTransport {
public:
    explicit SelfDeliveringTransport(std::string reply, std::string firstCallsTool = {})
        : LLMTransport(llm::LLMProvider::CUSTOM, llm::LLMConfig{}), _reply(std::move(reply)),
          _toolToCall(std::move(firstCallsTool)) {}
    std::string getName() const override { return "SelfDeliveringTransport"; }
    bool initialize() override {
        _initialized = true;
        return true;
    }

protected:
    std::string sendLLMRequest(const std::string&) override {
        if (!_toolToCall.empty()) {
            auto call = std::make_shared<transport::ToolCallFrame>();
            call->toolName = _toolToCall;
            call->argsJson = R"({"on": true})";
            call->sourceId = getName();
            _toolToCall.clear();
            fireOnFrameReceived(call);
            return "";
        }
        if (_reply.empty()) return "";
        addMessageToHistory(llm::MessageRole::ASSISTANT, _reply);
        auto frame = std::make_shared<transport::TextFrame>();
        frame->text = _reply;
        frame->isFinal = true;
        frame->sourceId = getName();
        fireOnFrameReceived(frame);
        return "";
    }

private:
    std::string _reply;
    std::string _toolToCall;
};

struct Said {
    std::string text;
    bool isFinal = false;
};

// What the LLM sends on to TTS
class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        auto text = std::dynamic_pointer_cast<transport::TextFrame>(frame);
        if (!text) return;
        std::lock_guard<std::mutex> lock(_mutex);
        _said.push_back({text->text, text->isFinal});
    }
    std::vector<Said> said() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _said;
    }
    size_t finalFrames() {
        size_t count = 0;
        for (const auto& frame : said()) count += frame.isFinal ? 1 : 0;
        return count;
    }

private:
    std::mutex _mutex;
    std::vector<Said> _said;
};

class LLMFailureReplyTest : public ::testing::Test {
protected:
    void startWith(const std::string& reply, const std::string& firstCallsTool = {}) {
        _llm = std::make_shared<llm::LLMProcessor>(std::make_shared<SelfDeliveringTransport>(reply, firstCallsTool));
        _llm->setStreamSpeech(true);
        _llm->setFailureReplies({"Sorry, I couldn't get an answer just now."});
        ASSERT_TRUE(_recorder->initialize());
        ASSERT_TRUE(_recorder->start());
        if (!firstCallsTool.empty()) {   // the tool runs between the LLM and what it says, as in a session
            _tools = std::make_shared<llm::ToolDispatchProcessor>();
            _tools->registerTool(llm::Tool(firstCallsTool, "Turns the flashlight on or off")
                                     .onCall([this](const nlohmann::json&) {
                                         ++_toolCalls;
                                         return nlohmann::json{{"flashlight", "on"}};
                                     }));
            _llm->setToolDispatcher(_tools);
            _llm->setDownstream(_tools);
            _tools->setDownstream(_recorder);
            ASSERT_TRUE(_tools->initialize());
            ASSERT_TRUE(_tools->start());
        } else {
            _llm->setDownstream(_recorder);
        }
        ASSERT_TRUE(_llm->initialize());
        ASSERT_TRUE(_llm->start());
    }
    void userSays(const std::string& text) {
        auto turn = std::make_shared<transport::UserTurnFrame>();
        turn->text = text;
        turn->confidence = 1.0f;
        turn->turnId = ++_turnId;
        _llm->processFrame(turn, pipeline::FlowDirection::DOWNSTREAM);
    }
    std::string spoken() {
        std::string text;
        for (const auto& frame : _recorder->said()) text += frame.text;
        return text;
    }
    void TearDown() override {
        if (_llm) _llm->stop();
        if (_tools) _tools->stop();
        _recorder->stop();
    }

    std::atomic<int> _toolCalls{0};
    std::shared_ptr<Recorder> _recorder = std::make_shared<Recorder>();
    std::shared_ptr<llm::ToolDispatchProcessor> _tools;
    std::shared_ptr<llm::LLMProcessor> _llm;
    uint64_t _turnId = 0;
};

TEST_F(LLMFailureReplyTest, ReplyDeliveredByTheTransportIsSaidOnce) {
    startWith("Hello! How can I help you today?");
    userSays("Hi.");
    ASSERT_TRUE(waitUntil([&] { return _recorder->finalFrames() >= 1; })) << "the reply reaches TTS";
    std::this_thread::sleep_for(200ms);   // fixed wait: checking that no failure reply follows
    EXPECT_EQ(_recorder->finalFrames(), 1u) << "one reply, no failure reply after it";
    EXPECT_EQ(spoken(), "Hello! How can I help you today?");
}

TEST_F(LLMFailureReplyTest, EveryTurnAnsweredByTheTransportIsSaidOnce) {
    startWith("Sure.");
    userSays("Hi.");
    ASSERT_TRUE(waitUntil([&] { return _recorder->finalFrames() >= 1; }));
    userSays("Thanks.");
    ASSERT_TRUE(waitUntil([&] { return _recorder->finalFrames() >= 2; }));
    std::this_thread::sleep_for(200ms);   // fixed wait: checking that no failure reply follows
    EXPECT_EQ(_recorder->finalFrames(), 2u) << "one reply per turn";
    EXPECT_EQ(spoken(), "Sure.Sure.");
}

// A native tool call (llama.cpp's LFM2 tool tokens) answers the turn: the tool runs, and its
// follow-up reply is what's said - no failure reply before it.
TEST_F(LLMFailureReplyTest, NativeToolCallIsNotAFailure) {
    startWith("The flashlight is on.", "set_flashlight");
    userSays("Turn on the flashlight.");
    ASSERT_TRUE(waitUntil([&] { return _recorder->finalFrames() >= 1; })) << "the follow-up reaches TTS";
    std::this_thread::sleep_for(200ms);   // fixed wait: checking that nothing else is said
    EXPECT_EQ(_toolCalls.load(), 1) << "the tool ran once";
    EXPECT_EQ(_recorder->finalFrames(), 1u) << "one reply, the follow-up";
    EXPECT_EQ(spoken(), "The flashlight is on.");
}

TEST_F(LLMFailureReplyTest, NoAnswerGetsTheFailureReply) {
    startWith("");
    userSays("Hi.");
    ASSERT_TRUE(waitUntil([&] { return _recorder->finalFrames() >= 1; })) << "the failure reply reaches TTS";
    EXPECT_EQ(spoken(), "Sorry, I couldn't get an answer just now.");
}

} // namespace
