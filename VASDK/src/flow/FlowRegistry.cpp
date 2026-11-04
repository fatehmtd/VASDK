#include "flow/FlowRegistry.h"

#include "flow/CompiledAgent.h"
#include "flow/SlotExtraction.h"
#include "vasdk/flow/page_types.pb.h"

#include <algorithm>

namespace va {
namespace flow {

namespace {

using Message = google::protobuf::Message;

// Whether `phrase` is in the words, longer than the longest found so far.
bool saysWithin(const std::vector<std::string>& words, const std::string& phrase, size_t& longest) {
    const auto phraseWords = splitWords(phrase);
    if (phraseWords.empty() || phraseWords.size() <= longest ||
        std::search(words.begin(), words.end(), phraseWords.begin(), phraseWords.end()) == words.end())
        return false;
    longest = phraseWords.size();
    return true;
}

// The option the user named: its value said whole (a button, or a choice matched), else the
// longest label, value or synonym found in the words.
const vasdk::flow::MenuOption* chosenOption(const vasdk::flow::MenuParams& menu, const std::string& text) {
    auto valueOf = [](const vasdk::flow::MenuOption& option) { return option.value().empty() ? option.label() : option.value(); };
    for (const auto& option : menu.options())
        if (valueOf(option) == text) return &option;
    const auto words = splitWords(text);
    const vasdk::flow::MenuOption* best = nullptr;
    size_t longest = 0;
    for (const auto& option : menu.options()) {
        bool named = saysWithin(words, option.label(), longest);
        named = saysWithin(words, valueOf(option), longest) || named;
        for (const auto& synonym : option.synonyms()) named = saysWithin(words, synonym, longest) || named;
        if (named) best = &option;
    }
    return best;
}

void addMenu(FlowRegistry& registry) {
    using vasdk::flow::MenuParams;
    FlowRegistry::PageType menu;
    menu.paramsType = MenuParams::descriptor()->full_name();
    menu.onEnter = [](const Message& params, TurnContext& turn) {
        FlowRegistry::withParams<MenuParams>(params, [&](const MenuParams& menuParams) {
            std::vector<vasdk::flow::Choice> choices;
            for (const auto& option : menuParams.options()) {
                vasdk::flow::Choice choice;
                choice.set_label(option.label());
                choice.set_value(option.value());
                *choice.mutable_synonyms() = option.synonyms();
                choices.push_back(std::move(choice));
            }
            turn.ask(turn.render(menuParams.prompt()), choices);
        });
    };
    menu.onTurn = [](const Message& params, TurnContext& turn) {
        bool answered = false;
        FlowRegistry::withParams<MenuParams>(params, [&](const MenuParams& menuParams) {
            const auto* option = chosenOption(menuParams, turn.text());
            if (option && !option->target().empty()) {
                turn.goTo(option->target());
                answered = true;
            }
        });
        return answered;
    };
    menu.targets = [](const Message& params) {
        std::vector<std::string> targets;
        FlowRegistry::withParams<MenuParams>(params, [&](const MenuParams& menuParams) {
            for (const auto& option : menuParams.options()) targets.push_back(option.target());
        });
        return targets;
    };
    registry.addPageType("menu", std::move(menu));
}

void addYesNo(FlowRegistry& registry) {
    using vasdk::flow::YesNoParams;
    FlowRegistry::PageType yesNo;
    yesNo.paramsType = YesNoParams::descriptor()->full_name();
    yesNo.onEnter = [](const Message& params, TurnContext& turn) {
        FlowRegistry::withParams<YesNoParams>(params, [&](const YesNoParams& question) {
            vasdk::flow::Expectation expect;
            expect.set_mode(vasdk::flow::Expectation::YES_NO);
            turn.ask(turn.render(question.question()), {}, expect);
        });
    };
    yesNo.onTurn = [](const Message& params, TurnContext& turn) {
        bool answered = false;
        FlowRegistry::withParams<YesNoParams>(params, [&](const YesNoParams& question) {
            const auto answer = extractYesNo(turn.text());
            const std::string target = !answer ? "" : *answer ? question.yes_target() : question.no_target();
            if (!target.empty()) {
                turn.goTo(target);
                answered = true;
            }
        });
        return answered;
    };
    yesNo.targets = [](const Message& params) {
        std::vector<std::string> targets;
        FlowRegistry::withParams<YesNoParams>(params, [&](const YesNoParams& question) {
            targets = {question.yes_target(), question.no_target()};
        });
        return targets;
    };
    registry.addPageType("yes_no", std::move(yesNo));
}

} // namespace

FlowRegistry::FlowRegistry() {
    addMenu(*this);
    addYesNo(*this);
    addPageType<vasdk::flow::EndParams>(
        "end",
        [](const vasdk::flow::EndParams& end, TurnContext& turn) {
            turn.say(turn.render(end.text()));
            turn.goTo(kTargetEndSession);
        },
        nullptr);
}

std::shared_ptr<const FlowRegistry> FlowRegistry::defaults() {
    static const auto registry = std::make_shared<const FlowRegistry>();
    return registry;
}

void FlowRegistry::addStep(const std::string& paramsType, Step step) { _steps[paramsType] = std::move(step); }
void FlowRegistry::addPageType(const std::string& name, PageType type) { _pageTypes[name] = std::move(type); }
void FlowRegistry::addEnvelope(const std::string& name, Envelope envelope) { _envelopes[name] = std::move(envelope); }
void FlowRegistry::addTool(llm::Tool tool) {
    const std::string name = tool.name();
    _tools[name] = std::move(tool);
}

const FlowRegistry::Step* FlowRegistry::step(const std::string& paramsType) const {
    const auto found = _steps.find(paramsType);
    return found == _steps.end() ? nullptr : &found->second;
}

const FlowRegistry::PageType* FlowRegistry::pageType(const std::string& name) const {
    const auto found = _pageTypes.find(name);
    return found == _pageTypes.end() ? nullptr : &found->second;
}

const FlowRegistry::Envelope* FlowRegistry::envelope(const std::string& name) const {
    const auto found = _envelopes.find(name);
    return found == _envelopes.end() ? nullptr : &found->second;
}

const llm::Tool* FlowRegistry::tool(const std::string& name) const {
    const auto found = _tools.find(name);
    return found == _tools.end() ? nullptr : &found->second;
}

} // namespace flow
} // namespace va
