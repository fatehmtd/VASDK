#include "flow/FlowTestRunner.h"

#include "engine/ProtoFileLoader.h"
#include "flow/FlowRuntime.h"

#include <algorithm>
#include <cctype>

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
    if (testCase.has_context()) {
        const auto context = testCase.context();
        conversation.setContextSource([context] { return context; });
    }
    for (int i = 0; i < testCase.turns_size(); ++i) {
        const auto& turn = testCase.turns(i);
        const Reply reply = turn.user().empty() ? conversation.start() : conversation.handleText(turn.user());
        const std::string where = "turn " + std::to_string(i + 1) + " " +
                                  (turn.user().empty() ? std::string("(start)") : quoted(turn.user()));
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
            const std::string actual = toText(conversation.sessionParam(name));
            if (actual != expected) fail("$session." + name + " is " + quoted(actual) + ", expected " + quoted(expected));
        }
        for (const auto& [name, expected] : expect.entities()) {
            const std::string actual = toText(conversation.lookup({"nlu", "entities", name}));
            if (actual != expected) fail("$nlu.entities." + name + " is " + quoted(actual) + ", expected " + quoted(expected));
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
