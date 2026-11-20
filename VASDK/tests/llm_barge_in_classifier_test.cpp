// LlmBargeInClassifier: what it tells the model about speech over the agent (or while it thinks),
// and how it reads the model's one-word answer. No answer, or an unreadable one, is Undecided.

#include <VASDK/llm/LlmBargeInClassifier.h>

#include <gtest/gtest.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

using va::llm::ConversationMessage;
using va::llm::LlmBargeInClassifier;
using va::llm::MessageRole;
using va::pipeline::BackchannelVerdict;
using va::pipeline::BargeInContext;

namespace va::pipeline {
void PrintTo(BackchannelVerdict verdict, std::ostream* out) {
    *out << (verdict == BackchannelVerdict::Interrupt ? "Interrupt"
             : verdict == BackchannelVerdict::Backchannel ? "Backchannel" : "Undecided");
}
} // namespace va::pipeline

namespace {

// Answers with a fixed reply (none: a failure or a timeout) and keeps what it was asked.
class FakeCompletion : public va::llm::Completion {
public:
    explicit FakeCompletion(std::optional<std::string> reply) : _reply(std::move(reply)) {}
    std::optional<std::string> complete(const std::vector<ConversationMessage>& messages) const override {
        return completeWithin(messages, 0);
    }
    std::optional<std::string> completeWithin(const std::vector<ConversationMessage>& messages, int timeLimitMs) const override {
        std::lock_guard<std::mutex> lock(_mutex);
        _asked = messages;
        _timeLimitMs = timeLimitMs;
        return _reply;
    }
    std::vector<ConversationMessage> asked() const { std::lock_guard<std::mutex> lock(_mutex); return _asked; }
    int timeLimitMs() const { std::lock_guard<std::mutex> lock(_mutex); return _timeLimitMs; }

private:
    mutable std::mutex _mutex;
    std::optional<std::string> _reply;
    mutable std::vector<ConversationMessage> _asked;
    mutable int _timeLimitMs = -1;
};

BargeInContext speakingContext(const std::string& heard) {
    BargeInContext context;
    context.heard = heard;
    context.isFinal = true;
    context.agentReply = "I have a table for four on Tuesday at 7 pm. Shall I confirm it?";
    context.lastUserTurn = "Book a table for Tuesday";
    return context;
}

BargeInContext thinkingContext(const std::string& heard) {
    BargeInContext context;
    context.heard = heard;
    context.isFinal = true;
    context.agentIsThinking = true;
    context.lastUserTurn = "Book a table for Tuesday";
    return context;
}

BackchannelVerdict classifyWith(std::optional<std::string> reply, const BargeInContext& context) {
    LlmBargeInClassifier classifier(std::make_shared<FakeCompletion>(std::move(reply)), 600);
    return classifier.classify(context);
}

} // namespace

TEST(LlmBargeInClassifier, ReadsTheSpeakingAnswerWords) {
    EXPECT_EQ(classifyWith("INTERRUPT", speakingContext("sorry, which day?")), BackchannelVerdict::Interrupt);
    EXPECT_EQ(classifyWith("backchannel.", speakingContext("Tuesday, right")), BackchannelVerdict::Backchannel)
        << "case and punctuation are ignored";
}

TEST(LlmBargeInClassifier, ReadsTheThinkingAnswerWords) {
    EXPECT_EQ(classifyWith("ADDS", thinkingContext("for four people")), BackchannelVerdict::Interrupt);
    EXPECT_EQ(classifyWith("acknowledges", thinkingContext("okay")), BackchannelVerdict::Backchannel);
    EXPECT_EQ(classifyWith("BACKCHANNEL", thinkingContext("okay")), BackchannelVerdict::Undecided)
        << "while thinking only ADDS or ACKNOWLEDGES count";
}

TEST(LlmBargeInClassifier, TheLastAnswerWordCounts) {
    EXPECT_EQ(LlmBargeInClassifier::verdictOf("It could be INTERRUPT, but the user agrees: BACKCHANNEL", false),
              BackchannelVerdict::Backchannel) << "a model that reasons first answers last";
}

TEST(LlmBargeInClassifier, NoOrUnreadableAnswerIsUndecided) {
    EXPECT_EQ(classifyWith(std::nullopt, speakingContext("which day")), BackchannelVerdict::Undecided)
        << "a failure or a timeout leaves it to the word rules";
    EXPECT_EQ(classifyWith("I am not sure.", speakingContext("which day")), BackchannelVerdict::Undecided);
    EXPECT_EQ(classifyWith("INTERRUPTION", speakingContext("which day")), BackchannelVerdict::Undecided)
        << "only the exact words count";
}

TEST(LlmBargeInClassifier, NothingHeardAsksNothing) {
    auto completion = std::make_shared<FakeCompletion>("INTERRUPT");
    LlmBargeInClassifier classifier(completion, 600);
    EXPECT_EQ(classifier.classify(speakingContext("  ")), BackchannelVerdict::Undecided);
    EXPECT_TRUE(completion->asked().empty()) << "no words: the model isn't asked";
}

TEST(LlmBargeInClassifier, TellsTheModelTheContextWithinTheTimeLimit) {
    auto completion = std::make_shared<FakeCompletion>("BACKCHANNEL");
    LlmBargeInClassifier classifier(completion, 600);
    auto context = speakingContext("Tuesday, right");
    context.conversationSummary = "The user is booking a table.";
    classifier.classify(context);
    const auto asked = completion->asked();
    ASSERT_GE(asked.size(), 2u);
    EXPECT_NE(asked.front().content.find("BACKCHANNEL or INTERRUPT"), std::string::npos);
    const auto& judged = asked.back().content;   // after the worked examples
    EXPECT_NE(judged.find("Shall I confirm it?"), std::string::npos) << "the agent's reply";
    EXPECT_NE(judged.find("Tuesday, right"), std::string::npos) << "what the user said over it";
    EXPECT_NE(judged.find("Book a table for Tuesday"), std::string::npos) << "the user's previous turn";
    EXPECT_NE(judged.find("The user is booking a table."), std::string::npos) << "the summary, when there is one";
    EXPECT_EQ(completion->timeLimitMs(), 600);
}

TEST(LlmBargeInClassifier, WhileThinkingAsksAboutTheRequest) {
    const auto messages = LlmBargeInClassifier::messagesFor(thinkingContext("for four people"));
    ASSERT_GE(messages.size(), 2u);
    EXPECT_NE(messages.front().content.find("ADDS or ACKNOWLEDGES"), std::string::npos);
    for (size_t index = 1; index + 1 < messages.size(); index += 2) {   // worked examples: a question, its answer
        EXPECT_EQ(messages[index].role, MessageRole::USER);
        EXPECT_EQ(messages[index + 1].role, MessageRole::ASSISTANT);
    }
    const auto& judged = messages.back().content;
    EXPECT_NE(judged.find("The user asked: \"Book a table for Tuesday\""), std::string::npos);
    EXPECT_EQ(judged.find("The assistant is saying"), std::string::npos) << "nothing is said yet";
    EXPECT_EQ(judged.find("Conversation so far"), std::string::npos) << "no summary, no line for it";
    EXPECT_NE(judged.find("ADDS or ACKNOWLEDGES?"), std::string::npos) << "the choice last, so a small model labels it";
}
