#include "flow/FlowTestRunner.h"

#include "engine/ProtoFileLoader.h"
#include "flow/FlowRuntime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace va {
namespace flow {

namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}
std::string quoted(const std::string& text) { return "\"" + text + "\""; }
const char* yesNo(bool value) { return value ? "true" : "false"; }
} // namespace

FlowTestResult runFlowTest(std::shared_ptr<const CompiledAgent> agent, const vasdk::flow::FlowTestCase& testCase) {
    FlowTestResult result;
    result.name = testCase.name();
    FlowRuntime conversation(agent);
    for (const auto& [name, value] : testCase.session().fields()) {
        std::string error;
        if (!conversation.setSessionParam(name, value, &error)) result.failures.push_back("session: " + error);
    }
    for (const auto& entity : testCase.entities()) {
        std::string error;
        if (!conversation.setEntityValues(entity.name(), {entity.values().begin(), entity.values().end()}, &error))
            result.failures.push_back("entities: " + error);
    }
    // A turn's tool_results stand in for its tools; others run as usual (HTTP ones are refused:
    // a test has no engine config to allow them).
    auto mocks = std::make_shared<google::protobuf::Map<std::string, std::string>>();
    conversation.setToolCaller([mocks, runner = ToolRunner::caller(std::make_shared<ToolRunner>(agent))](
                                   const std::string& name, const nlohmann::json& args) {
        const auto mocked = mocks->find(name);
        if (mocked == mocks->end()) return runner(name, args);
        ToolResult result;
        auto value = nlohmann::json::parse(mocked->second, nullptr, false);
        if (value.is_discarded()) value = mocked->second;
        if (value.is_object() && value.contains("error")) {
            result.error = value["error"].is_string() ? value["error"].get<std::string>() : value["error"].dump();
        } else {
            result.ok = true;
            result.value = std::move(value);
        }
        return result;
    });
    // The clock triggers read: the case's start_time (local), moved on by each turn's after_ms.
    auto clock = std::make_shared<std::chrono::system_clock::time_point>(std::chrono::system_clock::now());
    if (!testCase.start_time().empty()) {
        std::tm local{};
        std::istringstream text(testCase.start_time());
        text >> std::get_time(&local, "%Y-%m-%dT%H:%M:%S");
        local.tm_isdst = -1;
        if (text.fail()) result.failures.push_back("start_time " + quoted(testCase.start_time()) + " isn't YYYY-MM-DDTHH:MM:SS");
        else *clock = std::chrono::system_clock::from_time_t(std::mktime(&local));
    }
    conversation.setClock([clock] { return *clock; });
    if (testCase.has_context()) {
        const auto context = testCase.context();
        conversation.setContextSource([context] { return context; });
    }
    for (int i = 0; i < testCase.turns_size(); ++i) {
        const auto& turn = testCase.turns(i);
        *mocks = turn.tool_results();
        *clock += std::chrono::milliseconds(turn.after_ms());
        const Reply reply = !turn.fire_trigger().empty() ? conversation.fireTrigger(turn.fire_trigger(), turn.trigger_payload())
                          : turn.no_input()        ? conversation.handleNoInput()
                          : turn.has_choice()     ? conversation.selectChoice(turn.choice())
                          : turn.user().empty()   ? conversation.start()
                                                  : conversation.handleText(turn.user());
        const std::string where = "turn " + std::to_string(i + 1) + " " +
                                  (!turn.fire_trigger().empty() ? "(trigger " + turn.fire_trigger() + ")"
                                   : turn.has_choice()  ? "(choice " + std::to_string(turn.choice()) + ")"
                                   : turn.user().empty() ? std::string("(start)")
                                                         : quoted(turn.user()));
        auto fail = [&](const std::string& message) { result.failures.push_back(where + ": " + message); };
        const auto& expect = turn.expect();
        if (!expect.page().empty() && conversation.pageName() != expect.page())
            fail("page is " + quoted(conversation.pageName()) + ", expected " + quoted(expect.page()));
        if (!expect.flow().empty() && conversation.flowName() != expect.flow())
            fail("flow is " + quoted(conversation.flowName()) + ", expected " + quoted(expect.flow()));
        if (!expect.said().empty() && reply.text != expect.said())
            fail("said " + quoted(reply.text) + ", expected " + quoted(expect.said()));
        for (const auto& part : expect.said_contains())
            if (lower(reply.text).find(lower(part)) == std::string::npos)
                fail("said " + quoted(reply.text) + ", which doesn't contain " + quoted(part));
        if (expect.has_matched() && reply.matched != expect.matched())
            fail(std::string("matched is ") + yesNo(reply.matched) + ", expected " + yesNo(expect.matched()));
        if (expect.has_expects_reply() && reply.expectsReply != expect.expects_reply())
            fail(std::string("expects_reply is ") + yesNo(reply.expectsReply) + ", expected " +
                 yesNo(expect.expects_reply()));
        if (expect.has_ends_session() && reply.endsSession != expect.ends_session())
            fail(std::string("ends_session is ") + yesNo(reply.endsSession) + ", expected " +
                 yesNo(expect.ends_session()));
        for (const auto& [name, expected] : expect.session()) {
            Path path{"session"};
            for (size_t start = 0, dot; start <= name.size(); start = dot + 1) {   // "booking.reference": a field of a STRUCT param
                dot = std::min(name.find('.', start), name.size());
                path.push_back(name.substr(start, dot - start));
            }
            const std::string actual = toText(conversation.lookup(path));
            if (actual != expected) fail("$session." + name + " is " + quoted(actual) + ", expected " + quoted(expected));
        }
        if (!expect.event().empty() && reply.event != expect.event())
            fail("event is " + quoted(reply.event) + ", expected " + quoted(expect.event()));
        if (!expect.choices().empty()) {
            std::string offered, expected;
            for (const auto& choice : reply.choices) offered += (offered.empty() ? "" : ", ") + quoted(choice.label);
            for (const auto& label : expect.choices()) expected += (expected.empty() ? "" : ", ") + quoted(label);
            if (offered != expected) fail("choices are [" + offered + "], expected [" + expected + "]");
        }
        using Expectation = vasdk::flow::Expectation;
        if (expect.expect_mode() != Expectation::MODE_UNSET && reply.expectation.mode() != expect.expect_mode())
            fail("expects " + Expectation::Mode_Name(reply.expectation.mode()) + ", expected " +
                 Expectation::Mode_Name(expect.expect_mode()));
        if (expect.keyboard() != Expectation::KEYBOARD_UNSET && reply.expectation.keyboard() != expect.keyboard())
            fail("keyboard is " + Expectation::Keyboard_Name(reply.expectation.keyboard()) + ", expected " +
                 Expectation::Keyboard_Name(expect.keyboard()));
        if (expect.has_follow_up_listen_ms() && reply.followUpListenMs != expect.follow_up_listen_ms())
            fail("follow_up_listen_ms is " + (reply.followUpListenMs ? std::to_string(*reply.followUpListenMs) : "unset") +
                 ", expected " + std::to_string(expect.follow_up_listen_ms()));
        for (const auto& [name, expected] : expect.entities()) {
            const std::string actual = toText(conversation.lookup({"nlu", "entities", name}));
            if (actual != expected) fail("$nlu.entities." + name + " is " + quoted(actual) + ", expected " + quoted(expected));
        }
        if (expect.trigger_decision() != vasdk::flow::DECISION_UNSET) {
            const auto decision = reply.triggers.empty() ? vasdk::flow::DECISION_UNSET : reply.triggers.front().decision;
            if (decision != expect.trigger_decision())
                fail("the trigger was " + vasdk::flow::TriggerDecision_Name(decision) + ", expected " +
                     vasdk::flow::TriggerDecision_Name(expect.trigger_decision()));
        }
        if (!expect.tools_called().empty()) {
            std::string called, expected;
            for (const auto& call : reply.toolCalls) called += (called.empty() ? "" : ", ") + quoted(call.name);
            for (const auto& name : expect.tools_called()) expected += (expected.empty() ? "" : ", ") + quoted(name);
            if (called != expected) fail("tools called are [" + called + "], expected [" + expected + "]");
        }
        for (const auto& [name, expected] : expect.form()) {
            const std::string actual = toText(conversation.lookup({"form", name}));
            if (actual != expected) fail("$form." + name + " is " + quoted(actual) + ", expected " + quoted(expected));
        }
    }
    return result;
}

FlowTestResult runFlowTestFile(std::shared_ptr<const CompiledAgent> agent, const std::string& path) {
    vasdk::flow::FlowTestCase testCase;
    std::string error;
    if (!engine::loadProtoFile(path, testCase, &error)) {
        FlowTestResult result;
        result.name = path;
        result.failures.push_back(error);
        return result;
    }
    auto result = runFlowTest(std::move(agent), testCase);
    if (result.name.empty()) result.name = path;
    return result;
}

} // namespace flow
} // namespace va
