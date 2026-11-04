// Understanding without an LLM: intents from examples despite speech-recognition misspellings,
// entities inside examples ("book @service"), negation ("I don't want to cancel") and negative
// examples, the built-in sys.* intents, "did you mean...?" when two intents fit as well, fuzzy
// entity synonyms (unless turned off), and a cascade that asks a further stage only when the rules
// aren't sure.

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/nlu/NluProvider.h>

#include <google/protobuf/text_format.h>

#include <atomic>
#include <cstdio>
#include <string>

using namespace va;
using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

const char* kAgent = R"agent(
start_flow: "main"
intents { name: "book" examples: ["book", "book @service", "bring my car in"] }
intents { name: "cancel" examples: ["cancel", "cancel my appointment"] negative_examples: ["don't cancel"] }
intents { name: "hours" examples: ["opening hours", "when are you open"] }
intents { name: "tire_info" examples: ["about tires"] }
intents { name: "tire_price" examples: ["about tires"] }
entity_types { name: "service" list { values { value: "tires" synonyms: ["tires", "tire repair"] }
                                      values { value: "oil_change" synonyms: ["oil change"] } } }
entity_types { name: "code" list { values { value: "alpha" synonyms: ["promo code"] } } matching { fuzzy: false } }
flows { name: "main" start_page: "welcome"
  pages {
    name: "welcome"
    on_enter { ask { say { text: "How can I help?" } } }
    routes { intent: "book" steps { say { text: "Booking $nlu.confidence." } } }
    routes { intent: "cancel" steps { say { text: "Cancelling." } } }
    routes { intent: "hours" steps { say { text: "Open 8 to 6, by $nlu.stage." } } }
    routes { intent: "sys.help" steps { say { text: "Say book, cancel or hours." } } }
    routes { intent: "tire_info" steps { say { text: "Info." } } }
    routes { intent: "tire_price" steps { say { text: "Price." } } }
    routes { entity: "@code" steps { say { text: "Code taken." } } }
    handlers { event: "clarify" steps { say { text: "Did you mean $nlu.alternatives?" } } }
  }
}
)agent";

std::shared_ptr<const CompiledAgent> compile(const std::string& text, std::string* error) {
    vasdk::flow::Agent agent;
    if (!google::protobuf::TextFormat::ParseFromString(text, &agent)) {
        *error = "parse error";
        return nullptr;
    }
    return CompiledAgent::compile(agent, error);
}

// A stage after the rules: knows only that "is the shop open saturday" means hours.
class FakeModel : public nlu::NluProvider {
public:
    std::string name() const override { return "model"; }
    nlu::NluResult understand(const nlu::NluRequest& request) const override {
        ++calls;
        nlu::NluResult result;
        if (request.text.find("shop open") != std::string::npos) result.intents.push_back({"hours", 1.0f, 0.9f});
        return result;
    }
    mutable std::atomic<int> calls{0};
};

} // namespace

int main() {
    std::string error;
    auto agent = compile(kAgent, &error);
    check(agent != nullptr, "the agent compiles");
    if (!agent) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return 1;
    }
    auto say = [&](const std::string& words) {
        FlowRuntime conversation(agent);
        conversation.start();
        return conversation.handleText(words);
    };

    // Speech-recognition misspellings, in intents and entity synonyms.
    auto reply = say("I'd like to cancel my apointment");
    check(reply.intent == "cancel", "a misspelled word still matches");
    check(say("what are your openin hours").intent == "hours", "another misspelling");
    check(say("bok it").intent.empty(), "short words must be exact");
    reply = say("I want to book my tyres");
    check(reply.intent == "book" && reply.text.rfind("Booking 1", 0) == 0, "an entity in an example, its synonym misspelled");
    check(say("my promo kode").intent.empty() && say("my promo kode").text.rfind("Code taken.", 0) != 0 &&
              say("my promo code").text.rfind("Code taken.", 0) == 0,
          "fuzzy off for an entity: exact only");

    // Negation and negative examples.
    check(say("I don't want to cancel").intent.empty(), "negated: not the intent");
    check(say("don't cancel my appointment").intent.empty(), "a negative example");
    check(say("cancel please").intent == "cancel", "not negated");

    // Built-in sys.* intents, used without declaring them.
    check(say("what can you do").text.rfind("Say book, cancel or hours.", 0) == 0, "sys.help built in");
    nlu::NluRequest request;
    request.text = "sure thing";
    const auto understood = agent->rules()->understand(request);
    check(!understood.intents.empty() && understood.intents.front().intent == "sys.yes", "sys.yes built in");

    // Two intents as likely: "did you mean...?" when the agent says how.
    reply = say("tell me about tires");
    check(reply.event == "clarify" && reply.text.rfind("Did you mean ", 0) == 0 && reply.text.find("tire_info") != std::string::npos &&
              reply.text.find("tire_price") != std::string::npos,
          "clarify between tied intents");
    if (reply.event != "clarify") std::fprintf(stderr, "  %s / %s\n", reply.event.c_str(), reply.text.c_str());

    // A cascade: the rules first; a further stage only when they aren't sure.
    auto model = std::make_shared<FakeModel>();
    auto cascade = std::make_shared<nlu::CascadeNluProvider>(
        std::vector<std::shared_ptr<const nlu::NluProvider>>{agent->rules(), model}, 0.75f);
    FlowRuntime conversation(agent);
    conversation.setNlu(cascade);
    conversation.start();
    reply = conversation.handleText("is the shop open saturday");
    check(reply.intent == "hours" && reply.text.rfind("Open 8 to 6, by model.", 0) == 0, "the next stage when the rules find nothing");
    const int callsBefore = model->calls;
    conversation.handleText("cancel");
    check(model->calls == callsBefore, "sure rules: the next stage isn't asked");

    check(nlu::wordSimilarity("tires", "tyres") == 0.8f && nlu::wordSimilarity("ab", "ba") == 0.5f, "similarity, a swap counts once");

    // Load errors
    std::string text = kAgent;
    text.replace(text.find("book @service"), std::string("book @service").size(), "book @servise");
    check(!compile(text, &error) && error.find("unknown entity \"@servise\"") != std::string::npos, "an unknown entity in an example");

    if (failures == 0) std::printf("rule_nlu_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
