// LlamaCppTransport on a real GGUF model (LFM2.5 by default, a hybrid model with a recurrent
// state), driven as a session drives it:
//   - through LLMProcessor, an answer is said once: no "couldn't get an answer" after it, though
//     the transport delivers its reply as frames and returns "" from sendLLMRequest;
//   - a turn cancelled mid-answer (a barge-in) leaves the conversation as it was: the next turn
//     is answered with the earlier exchange still in the history, none of it dropped;
//   - a system prompt and tools given after construction (as the catalog's transport gets them)
//     reach the model, which calls the tool; a new system prompt between turns takes effect.
// Skipped without the model.

#include "LlamaCpp/LlamaCppTransport.h"

#include <VASDK/llm/LLMProcessor.h>
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

constexpr const char* kFailureReply = "Sorry, I couldn't get an answer just now.";

llm::LLMConfig modelConfig() {
    llm::LLMConfig config;
    config.model = (va::test::modelsDirectory() / VASDK_LLAMACPP_TEST_MODEL_NAME).string();
    config.systemPrompt = "You are a helpful voice assistant. Answer briefly.";
    config.temperature = 0.01f;   // as good as greedy: 0 means the transport's default, 0.7
    config.maxTokens = 200;
    config.nCtx = 2048;
    return config;
}

std::vector<std::string> userMessages(const llm::LLMTransport& transport) {
    std::vector<std::string> said;
    for (const auto& message : transport.getHistory())
        if (message.role == llm::MessageRole::USER) said.push_back(message.content);
    return said;
}

// What the LLM sends on to TTS
class Recorder : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Recorder"; }
    void processFrame(std::shared_ptr<transport::Frame> frame, pipeline::FlowDirection) override {
        auto text = std::dynamic_pointer_cast<transport::TextFrame>(frame);
        if (!text) return;
        std::lock_guard<std::mutex> lock(_mutex);
        _spoken += text->text;
        _finalFrames += text->isFinal ? 1 : 0;
    }
    std::string spoken() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _spoken;
    }
    size_t finalFrames() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _finalFrames;
    }

private:
    std::mutex _mutex;
    std::string _spoken;
    size_t _finalFrames = 0;
};

TEST(LlamaCppTransport, AnswerIsSaidOnceThroughTheProcessor) {
    VA_REQUIRE_FILE(modelConfig().model);
    auto recorder = std::make_shared<Recorder>();
    auto llm = std::make_shared<llm::LLMProcessor>(std::make_shared<thirdparty::llm::LlamaCppTransport>(modelConfig()));
    llm->setStreamSpeech(true);
    llm->setFailureReplies({kFailureReply});
    llm->setDownstream(recorder);
    ASSERT_TRUE(recorder->initialize() && recorder->start());
    ASSERT_TRUE(llm->initialize()) << "the model loads";
    ASSERT_TRUE(llm->start());

    for (const uint64_t turnId : {1u, 2u}) {
        auto turn = std::make_shared<transport::UserTurnFrame>();
        turn->text = turnId == 1 ? "Hi there." : "What can you do?";
        turn->confidence = 1.0f;
        turn->turnId = turnId;
        llm->processFrame(turn, pipeline::FlowDirection::DOWNSTREAM);
        ASSERT_TRUE(waitUntil([&] { return recorder->finalFrames() >= turnId; }, 120s)) << "turn " << turnId << " is answered";
    }
    std::this_thread::sleep_for(300ms);   // fixed wait: checking that no failure reply follows
    EXPECT_EQ(recorder->finalFrames(), 2u) << "one reply per turn";
    EXPECT_EQ(recorder->spoken().find(kFailureReply), std::string::npos) << "said: " << recorder->spoken();
    EXPECT_FALSE(recorder->spoken().empty());
    llm->stop();
    recorder->stop();
}

TEST(LlamaCppTransport, CancelledTurnKeepsTheConversation) {
    VA_REQUIRE_FILE(modelConfig().model);
    thirdparty::llm::LlamaCppTransport transport(modelConfig());
    transport.setChunker(std::make_shared<transport::PunctuationChunker>());   // sentences as they come, as a session has
    ASSERT_TRUE(transport.initialize()) << "the model loads";

    std::atomic<bool> cancelMidAnswer{false};
    std::atomic<bool> cancelled{false};
    std::atomic<int> finalFrames{0};
    transport.onFrameReceived = [&](std::shared_ptr<transport::Frame> frame) {
        auto text = std::dynamic_pointer_cast<transport::TextFrame>(frame);
        if (!text) return;
        if (text->isFinal) ++finalFrames;
        else if (cancelMidAnswer && !cancelled.exchange(true)) transport.cancel();   // the user cuts in
    };
    auto say = [&](const std::string& text) {
        auto frame = std::make_shared<transport::TranscriptionFrame>();
        frame->isFinal = true;
        frame->transcriptionResult.text = text;
        frame->transcriptionResult.confidence = 1.0f;
        transport.sendFrame(frame);
    };

    say("My name is Ada.");
    ASSERT_EQ(finalFrames.load(), 1) << "the first turn is answered";

    cancelMidAnswer = true;
    say("Tell me a long story about a dragon, in at least ten sentences.");
    if (!cancelled) GTEST_SKIP() << "the model answered in one piece: no sentence to cut in after";
    EXPECT_EQ(finalFrames.load(), 1) << "the cancelled turn says nothing more";
    cancelMidAnswer = false;

    say("What is my name?");
    EXPECT_EQ(finalFrames.load(), 2) << "the turn after the cancelled one is answered";
    EXPECT_EQ(userMessages(transport), (std::vector<std::string>{"My name is Ada.", "What is my name?"}))
        << "the cancelled turn is rolled back, and the exchange before it kept";
    transport.release();
}

// The transport sends `text` as the user; what came back: the tools called, and the text said
struct Reply {
    std::vector<std::string> tools;
    std::string text;
};
Reply ask(thirdparty::llm::LlamaCppTransport& transport, const std::string& text) {
    Reply reply;
    transport.onFrameReceived = [&](std::shared_ptr<transport::Frame> frame) {
        if (auto call = std::dynamic_pointer_cast<transport::ToolCallFrame>(frame)) reply.tools.push_back(call->toolName);
        else if (auto said = std::dynamic_pointer_cast<transport::TextFrame>(frame)) reply.text += said->text;
    };
    auto frame = std::make_shared<transport::TranscriptionFrame>();
    frame->isFinal = true;
    frame->transcriptionResult.text = text;
    frame->transcriptionResult.confidence = 1.0f;
    transport.sendFrame(frame);
    return reply;
}

// As the catalog makes it: no prompt at first, then the system prompt and the tools' descriptions
// through setConfig. The model sees them, and calls the tool asked for.
TEST(LlamaCppTransport, PromptGivenAfterwardsReachesTheModel) {
    auto config = modelConfig();
    VA_REQUIRE_FILE(config.model);
    const std::string systemPrompt = config.systemPrompt;
    config.systemPrompt.clear();
    thirdparty::llm::LlamaCppTransport transport(config);
    transport.setPromptChatFormat(llm::ChatFormat::LFM2_5);
    ASSERT_TRUE(transport.initialize()) << "the model loads";

    llm::ToolDispatchProcessor tools;
    tools.registerTool(llm::Tool("set_flashlight", "Turn the flashlight (torch) on or off.")
                           .addParameter({"on", "true turns the flashlight on, false turns it off", "boolean", true}));
    auto prompted = transport.getConfig();
    prompted.systemPrompt = systemPrompt + "\n\n" + tools.renderToolDescriptions(llm::ChatFormat::LFM2_5);
    transport.setConfig(prompted);
    ASSERT_FALSE(transport.getHistory().empty());
    EXPECT_EQ(transport.getHistory().front().role, llm::MessageRole::SYSTEM) << "the prompt is the first message";

    const auto reply = ask(transport, "Turn on the flashlight.");
    EXPECT_EQ(reply.tools, std::vector<std::string>{"set_flashlight"}) << "said instead: " << reply.text;
    transport.release();
}

// A new system prompt between turns: the next turn is answered under it, not from the cache of
// the old one.
TEST(LlamaCppTransport, NewSystemPromptTakesEffectMidConversation) {
    VA_REQUIRE_FILE(modelConfig().model);
    thirdparty::llm::LlamaCppTransport transport(modelConfig());
    transport.setPromptChatFormat(llm::ChatFormat::LFM2_5);
    ASSERT_TRUE(transport.initialize()) << "the model loads";
    ASSERT_FALSE(ask(transport, "Hi.").text.empty());

    auto changed = transport.getConfig();
    changed.systemPrompt = "Whatever the user says, answer with exactly one word: PINEAPPLE.";
    transport.setConfig(changed);
    const auto reply = ask(transport, "How are you?");
    EXPECT_NE(reply.text.find("PINEAPPLE"), std::string::npos) << "said: " << reply.text;
    transport.release();
}

} // namespace
