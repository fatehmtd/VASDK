// LLM assist: an LLM understands what the rules can't, answers on generative pages and in `llm`
// steps, and reads form answers the slots can't. It may only name the agent's intents, move to
// allowed targets and fill slots with values their entity reads; sensitive answers never reach
// it; turns the rules are sure of never ask it. Without an LLM the same agent answers as before.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/flow/FlowTestRunner.h>
#include <VASDK/llm/Completion.h>
#include <VASDK/network/HttpInterface.h>
#include <VASDK/nlu/LlmNluProvider.h>

#include <google/protobuf/text_format.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <string>

using namespace va;
using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

// An LLM that answers from a script, and remembers what it was asked.
struct ScriptedLlm : llm::Completion {
    std::function<std::string(const std::vector<llm::ConversationMessage>&)> answer;
    mutable int calls = 0;
    mutable std::vector<llm::ConversationMessage> lastMessages;
    std::optional<std::string> complete(const std::vector<llm::ConversationMessage>& messages) const override {
        ++calls;
        lastMessages = messages;
        if (!answer) return std::nullopt;
        return answer(messages);
    }
};
bool asksForIntent(const std::vector<llm::ConversationMessage>& messages) {
    return messages.front().content.find("The intents") != std::string::npos;
}
std::string lastUserWords(const std::vector<llm::ConversationMessage>& messages) {
    for (auto message = messages.rbegin(); message != messages.rend(); ++message)
        if (message->role == llm::MessageRole::USER) return message->content;
    return {};
}

struct FakeHttp : network::HttpInterface {
    int status = 200;
    std::string body, sentUrl, sentBody;
    std::map<std::string, std::string> sentHeaders;
    network::HttpResponse post(const std::string& url, const std::map<std::string, std::string>& headers,
                               const std::string& request, int, std::function<void(const std::string&)>) override {
        sentUrl = url;
        sentHeaders = headers;
        sentBody = request;
        network::HttpResponse response;
        response.statusCode = status;
        response.body = body;
        return response;
    }
    network::HttpResponse get(const std::string&, const std::map<std::string, std::string>&, int) override { return {}; }
    bool sendText(const std::string&, const std::map<std::string, std::string>&, const std::string&, int) override { return false; }
    bool sendBytes(const std::string&, const std::map<std::string, std::string>&, const std::vector<uint8_t>&, int) override {
        return false;
    }
};

const char* kAgent = R"agent(
name: "Acme Garage"
start_flow: "main"
intents { name: "book" examples: ["book", "book a service", "make an appointment"] }
intents { name: "hours" examples: ["opening hours", "when are you open"] }
intents { name: "chat" examples: ["let's chat", "car advice"] }
entity_types { name: "service" list { values { value: "oil_change" spoken: "an oil change" synonyms: ["oil change"] }
                                      values { value: "tires" spoken: "tire repair" synonyms: ["tire repair", "tires"] } } }
goals {
  name: "booking"
  slots { name: "service" entity: "@service" prompt: "Which service do you need?" }
  slots { name: "phone" type: PHONE sensitive: true prompt: "What's your number?" }
}
flows { name: "main" start_page: "welcome"
  pages { name: "welcome"
    on_enter { ask { say { text: "Hi, how can I help?" } } }
    routes { intent: "book" target: "booking" }
    routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }
    routes { intent: "chat" target: "advice" } }
  pages { name: "booking" form { goal: "booking" } routes { condition: "$form.complete" target: "done" } }
  pages { name: "done" on_enter { say { text: "Booked." } } on_enter { end {} } }
  pages { name: "advice" type: "generative"
    params { [type.googleapis.com/vasdk.flow.GenerativeParams] {
      instructions: "Answer briefly about car care, then offer to book." allowed_targets: ["booking"] } }
    on_enter { ask { say { text: "Ask me anything about your car." } } }
    routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } }
    handlers { event: "no-match" steps { say { text: "I can help you book or tell you our hours." } } } }
  pages { name: "step"
    on_enter { llm { instructions: "Greet the user." offline { say { text: "Hello there." } } } } } }
)agent";

std::shared_ptr<const CompiledAgent> compile(const vasdk::flow::Agent& definition) {
    std::string error;
    auto agent = CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    return agent;
}
vasdk::flow::Agent parse() {
    vasdk::flow::Agent definition;
    if (!google::protobuf::TextFormat::ParseFromString(kAgent, &definition)) check(false, "parse");
    return definition;
}

// The rules first, then the LLM.
void assist(FlowRuntime& conversation, const std::shared_ptr<const CompiledAgent>& agent,
            const std::shared_ptr<const ScriptedLlm>& llm) {
    conversation.setNlu(std::make_shared<nlu::CascadeNluProvider>(std::vector<std::shared_ptr<const nlu::NluProvider>>{
        agent->rules(), std::make_shared<nlu::LlmNluProvider>(llm, agent->definition().intents())}));
    conversation.setLlm(llm);
}

} // namespace

int main() {
    // The reply's JSON, wherever it is in the words.
    check(llm::jsonObjectIn("Sure! ```json\n{\"a\": \"}{\", \"b\": {\"c\": 1}}\n``` done") == "{\"a\": \"}{\", \"b\": {\"c\": 1}}",
          "the first balanced object, braces in strings ignored");
    check(llm::jsonObjectIn("no json here").empty(), "none: empty");

    // An OpenAI-compatible endpoint: the request, and the reply's text.
    {
        auto http = std::make_shared<FakeHttp>();
        http->body = R"({"choices": [{"message": {"content": "{\"intent\": \"book\"}"}}]})";
        llm::LLMConfig config;
        config.endpoint = "https://llm.example/v1/chat/completions";
        config.model = "small";
        config.apiKey = "test-key";
        llm::OpenAICompletion completion(config, http);
        const auto reply = completion.complete({{llm::MessageRole::SYSTEM, "classify", 0}, {llm::MessageRole::USER, "hi", 0}});
        check(reply && *reply == "{\"intent\": \"book\"}", "the reply's content");
        const auto sent = nlohmann::json::parse(http->sentBody);
        check(sent["model"] == "small" && sent["messages"].size() == 2 && sent["messages"][0]["role"] == "system" &&
                  sent["messages"][1]["content"] == "hi",
              "the model and messages are sent");
        check(http->sentHeaders["Authorization"] == "Bearer test-key" && http->sentUrl == config.endpoint, "to the endpoint, with the key");
        http->status = 500;
        check(!completion.complete({{llm::MessageRole::USER, "hi", 0}}), "an error status: no reply");
    }

    auto agent = compile(parse());
    if (!agent) return 1;

    // Understanding: only the agent's intents; anything else, or words it can't read, is none.
    {
        auto llm = std::make_shared<ScriptedLlm>();
        nlu::LlmNluProvider provider(llm, agent->definition().intents());
        nlu::NluRequest request;
        request.text = "my brakes squeal";
        llm->answer = [](const auto&) { return std::string(R"({"intent": "book", "confidence": 0.8})"); };
        auto understood = provider.understand(request);
        check(understood.intents.size() == 1 && understood.intents[0].intent == "book" && understood.stage == "llm",
              "an intent of the agent");
        check(llm->lastMessages.front().content.find("make an appointment") != std::string::npos, "shown with its examples");
        llm->answer = [](const auto&) { return std::string(R"({"intent": "fly_to_mars", "confidence": 0.99})"); };
        check(provider.understand(request).intents.empty(), "an intent the agent doesn't have: none");
        llm->answer = [](const auto&) { return std::string("I think they want to book"); };
        check(provider.understand(request).intents.empty(), "words it can't read: none");
        request.sensitive = true;
        const int calls = llm->calls;
        provider.understand(request);
        check(llm->calls == calls, "sensitive words aren't sent");
    }

    // Without an LLM, what the rules don't know isn't understood; with one, it's asked, and only then.
    {
        FlowRuntime offline(agent);
        offline.start();
        check(!offline.handleText("my brakes are squealing").matched, "offline: not understood");

        auto llm = std::make_shared<ScriptedLlm>();
        llm->answer = [](const auto& messages) {
            return std::string(asksForIntent(messages) ? R"({"intent": "book", "confidence": 0.8})" : "{}");
        };
        FlowRuntime online(agent);
        assist(online, agent, llm);
        online.start();
        online.handleText("book");
        check(online.pageName() == "booking" && llm->calls == 0, "words the rules are sure of: the LLM isn't asked");
        FlowRuntime again(agent);
        assist(again, agent, llm);
        again.start();
        const auto reply = again.handleText("my brakes are squealing");
        check(again.pageName() == "booking" && reply.intent == "book" && llm->calls == 1,
              "the LLM understood it: " + again.pageName() + " " + reply.intent + " " + std::to_string(llm->calls));
        check(toText(again.lookup({"nlu", "stage"})) == "llm", "$nlu.stage is llm");
    }

    // Forms: the LLM reads an answer the slot can't, but only values the slot's entity takes are kept.
    {
        auto llm = std::make_shared<ScriptedLlm>();
        std::string slotWords = "oil change";
        llm->answer = [&slotWords](const auto& messages) {
            if (asksForIntent(messages)) return std::string(R"({"intent": "none"})");
            return nlohmann::json{{"slots", {{"service", slotWords}}}}.dump();
        };
        FlowRuntime conversation(agent);
        assist(conversation, agent, llm);
        conversation.start();
        conversation.handleText("book");
        auto reply = conversation.handleText("the black slippery stuff in the engine needs replacing");
        check(toText(conversation.lookup({"form", "service"})) == "oil_change", "the LLM's words, read as the entity");
        check(reply.text == "What's your number?", "then the next slot: " + reply.text);
        check(llm->lastMessages.front().content.find("an oil change") != std::string::npos, "the slot's values are shown to it");

        // A sensitive slot: its answer never reaches the LLM.
        const int calls = llm->calls;
        reply = conversation.handleText("it's the one I always use");
        check(llm->calls == calls, "a sensitive answer isn't sent: " + reply.text);
        conversation.handleText("555 123 4567");
        check(conversation.ended(), "booked");

        FlowRuntime invent(agent);
        assist(invent, agent, llm);
        invent.start();
        invent.handleText("book");
        slotWords = "teleportation";
        reply = invent.handleText("beam my car to the moon");
        check(toText(invent.lookup({"form", "service"})).empty(), "a value the entity doesn't have isn't kept");
        check(reply.text.find("Which service") != std::string::npos, "and the slot is asked again: " + reply.text);
    }

    // A generative page: its routes first, then the LLM; it moves only to allowed targets.
    {
        auto llm = std::make_shared<ScriptedLlm>();
        nlohmann::json answer = {{"say", "Change the oil every 5000 miles. Want to book one?"}};
        llm->answer = [&answer](const auto& messages) {
            return asksForIntent(messages) ? std::string(R"({"intent": "none"})") : answer.dump();
        };
        FlowRuntime conversation(agent);
        assist(conversation, agent, llm);
        conversation.start();
        conversation.handleText("let's chat");
        check(conversation.pageName() == "advice", "on the generative page");
        auto reply = conversation.handleText("when are you open");
        check(reply.text.rfind("We're open 8 to 6.", 0) == 0, "its routes first: " + reply.text);
        reply = conversation.handleText("how often should I change my oil");
        check(reply.text == "Change the oil every 5000 miles. Want to book one?" && reply.expectsReply,
              "the LLM answers the rest: " + reply.text);
        check(llm->lastMessages.front().content.find("car care") != std::string::npos &&
                  llm->lastMessages.front().content.find("booking") != std::string::npos,
              "it's given the instructions and the allowed targets");
        check(lastUserWords(llm->lastMessages) == "how often should I change my oil" && llm->lastMessages.size() > 3,
              "and the conversation so far");
        answer = {{"say", "Let's go."}, {"goto", "done"}};
        conversation.handleText("just end it");
        check(conversation.pageName() == "advice", "a target it isn't allowed: ignored");
        answer = {{"say", "Sure."}, {"goto", "booking"}};
        conversation.handleText("yes book me in");
        check(conversation.pageName() == "booking", "an allowed target: moved");

        FlowRuntime offline(agent);
        offline.start();
        offline.handleText("let's chat");
        reply = offline.handleText("how often should I change my oil");
        check(reply.text.rfind("I can help you book or tell you our hours.", 0) == 0,
              "offline: the page's no-match handler: " + reply.text);
    }

    // An llm step: the LLM's words, or its offline steps without an LLM or when it fails.
    {
        auto definition = parse();
        definition.mutable_flows(0)->set_start_page("step");
        auto steps = compile(definition);
        if (!steps) return 1;
        FlowRuntime offline(steps);
        check(offline.start().text == "Hello there.", "offline steps without an LLM");
        auto llm = std::make_shared<ScriptedLlm>();
        llm->answer = [](const auto&) { return std::string(R"({"say": "Good morning!"})"); };
        FlowRuntime online(steps);
        online.setLlm(llm);
        check(online.start().text == "Good morning!", "the LLM's words with one");
        llm->answer = nullptr;   // it fails
        FlowRuntime failing(steps);
        failing.setLlm(llm);
        check(failing.start().text == "Hello there.", "and the offline steps when it fails");
    }

    // allowed_targets are checked when the agent loads.
    {
        auto definition = parse();
        definition.mutable_flows(0)->mutable_pages(4)->mutable_on_enter(0)->mutable_llm()->add_allowed_targets("nowhere");
        std::string error;
        check(!CompiledAgent::compile(definition, &error) && error.find("nowhere") != std::string::npos,
              "an unknown allowed target fails to load: " + error);
    }

    // The same test case passes with and without an LLM: what the rules are sure of doesn't change.
    {
        vasdk::flow::FlowTestCase testCase;
        google::protobuf::TextFormat::ParseFromString(R"(
            name: "book by the rules"
            turns { expect { page: "welcome" } }
            turns { user: "when are you open" expect { said_contains: "8 to 6" } }
            turns { user: "book a service" expect { page: "booking" said: "Which service do you need?" } }
            turns { user: "tire repair" expect { said: "What's your number?" } }
            turns { user: "555 123 4567" expect { said: "Booked." } })", &testCase);
        check(runFlowTest(agent, testCase).passed(), "without an LLM");
        auto llm = std::make_shared<ScriptedLlm>();
        llm->answer = [](const auto&) { return std::string(R"({"intent": "chat", "confidence": 1, "say": "Off topic!"})"); };
        const auto result = runFlowTest(agent, testCase, [&](FlowRuntime& conversation) { assist(conversation, agent, llm); });
        for (const auto& failure : result.failures) std::fprintf(stderr, "    %s\n", failure.c_str());
        check(result.passed() && llm->calls == 0, "with one: passes, and it's never asked");
    }

    // Sensitive values are masked in what the LLM reads.
    {
        auto llm = std::make_shared<ScriptedLlm>();
        llm->answer = [](const auto& messages) {
            return asksForIntent(messages) ? std::string(R"({"intent": "none"})") : std::string(R"({"say": "Noted."})");
        };
        auto definition = parse();
        auto* booking = definition.mutable_flows(0)->mutable_pages(1);
        booking->clear_routes();
        booking->set_type("generative");
        vasdk::flow::GenerativeParams params;
        params.set_instructions("Help with the booking.");
        booking->mutable_params()->PackFrom(params);
        auto generative = compile(definition);
        if (!generative) return 1;
        FlowRuntime conversation(generative);
        assist(conversation, generative, llm);
        conversation.start();
        conversation.handleText("book");
        conversation.handleText("tire repair");
        conversation.handleText("555 123 4567");
        conversation.handleText("is that all you need");
        std::string sent;
        for (const auto& message : llm->lastMessages) sent += message.content + "\n";
        check(llm->calls > 0 && sent.find("555 123 4567") == std::string::npos && sent.find("5551234567") == std::string::npos,
              "the phone number is masked: " + sent);
    }

    if (failures == 0) std::printf("llm_assist_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
