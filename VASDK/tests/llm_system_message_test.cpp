// LLMTransport's system message: the history's first message is the system prompt, with the
// response format instruction after it. A transport made without a prompt and given one later
// through setConfig (as the provider catalog makes llama.cpp's, and Session and the apps then
// prompt it) has it; a new prompt replaces it, the conversation after it kept; the instruction
// is added once, however often it is set; clearHistory keeps it; with neither, there is none.

#include <VASDK/llm/LLMTransport.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace va;

namespace {

llm::LLMConfig withPrompt(const std::string& systemPrompt) {
    llm::LLMConfig config;
    config.systemPrompt = systemPrompt;
    return config;
}

// Each message as "<role>: <content>"
std::vector<std::string> history(const llm::LLMTransport& transport) {
    std::vector<std::string> messages;
    for (const auto& message : transport.getHistory()) {
        const char* role = message.role == llm::MessageRole::SYSTEM ? "system"
                         : message.role == llm::MessageRole::USER   ? "user"
                                                                    : "assistant";
        messages.push_back(std::string(role) + ": " + message.content);
    }
    return messages;
}

using Messages = std::vector<std::string>;

TEST(LLMSystemMessage, SetConfigPromptsATransportMadeWithoutOne) {
    llm::LLMTransport transport(llm::LLMProvider::CUSTOM, llm::LLMConfig{});
    EXPECT_EQ(history(transport), Messages{});
    transport.setConfig(withPrompt("You can turn the flashlight on."));
    EXPECT_EQ(history(transport), Messages{"system: You can turn the flashlight on."});
}

TEST(LLMSystemMessage, SetConfigPutsThePromptBeforeTheConversation) {
    llm::LLMTransport transport(llm::LLMProvider::CUSTOM, llm::LLMConfig{});
    transport.addMessageToHistory(llm::MessageRole::USER, "Hi.");
    transport.addMessageToHistory(llm::MessageRole::ASSISTANT, "Hello!");
    transport.setConfig(withPrompt("Be brief."));
    EXPECT_EQ(history(transport), (Messages{"system: Be brief.", "user: Hi.", "assistant: Hello!"}));
}

TEST(LLMSystemMessage, NewPromptReplacesTheOldAndKeepsTheConversation) {
    llm::LLMTransport transport(llm::LLMProvider::CUSTOM, withPrompt("Be brief."));
    transport.addMessageToHistory(llm::MessageRole::USER, "Hi.");
    transport.setConfig(withPrompt("Be very brief."));
    EXPECT_EQ(history(transport), (Messages{"system: Be very brief.", "user: Hi."}));
}

TEST(LLMSystemMessage, FormatInstructionFollowsThePromptOnce) {
    llm::LLMTransport transport(llm::LLMProvider::CUSTOM, llm::LLMConfig{});
    transport.setResponseFormatInstruction("Reply in JSON.");
    EXPECT_EQ(history(transport), Messages{"system: Reply in JSON."}) << "an instruction alone is the system message";
    transport.setConfig(withPrompt("Be brief."));
    transport.setResponseFormatInstruction("Reply in JSON.");
    EXPECT_EQ(history(transport), Messages{"system: Be brief.\n\nReply in JSON."}) << "kept by setConfig, not repeated";
    transport.setResponseFormatInstruction({});
    EXPECT_EQ(history(transport), Messages{"system: Be brief."}) << "an empty instruction removes it";
}

TEST(LLMSystemMessage, ClearHistoryKeepsTheSystemMessage) {
    llm::LLMTransport transport(llm::LLMProvider::CUSTOM, llm::LLMConfig{});
    transport.setConfig(withPrompt("Be brief."));
    transport.setResponseFormatInstruction("Reply in JSON.");
    transport.addMessageToHistory(llm::MessageRole::USER, "Hi.");
    transport.clearHistory();
    EXPECT_EQ(history(transport), Messages{"system: Be brief.\n\nReply in JSON."});
}

TEST(LLMSystemMessage, NoPromptNoSystemMessage) {
    llm::LLMTransport transport(llm::LLMProvider::CUSTOM, withPrompt("Be brief."));
    transport.addMessageToHistory(llm::MessageRole::USER, "Hi.");
    transport.setConfig(llm::LLMConfig{});
    EXPECT_EQ(history(transport), Messages{"user: Hi."});
}

} // namespace
