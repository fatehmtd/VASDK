#pragma once

// For a vertical's own test (verticals/<name>/<name>_test.cpp): the whole car and its agent, and a
// conversation wired as VASDK_Car wires its session. $context.car is the car's state, and what the
// verticals ask of the conversation (a trigger, a session param) reaches it.

#include "Car.h"

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/FlowRuntime.h>
#include <google/protobuf/util/json_util.h>

#include <cstdio>
#include <memory>
#include <string>

namespace car {

class CarTest {
public:
    /** argv[1]: the agent file. False, with the reason printed, when it doesn't load. */
    bool load(int argc, char** argv) {
        if (argc < 2) {
            std::fprintf(stderr, "usage: %s <agent.txtpb>\n", argv[0]);
            return false;
        }
        vehicle = makeCar();
        auto registry = std::make_shared<va::flow::FlowRegistry>();
        vehicle->registerWith(*registry);
        std::string error;
        agent = va::flow::CompiledAgent::load(argv[1], &error, nullptr, registry);
        if (!agent) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        drive = std::make_unique<va::flow::FlowRuntime>(agent);
        drive->setContextSource([car = vehicle] {
            google::protobuf::Struct context;
            (void)google::protobuf::util::JsonStringToMessage(nlohmann::json{{"car", car->state()}}.dump(), &context);
            return context;
        });
        Conversation conversation;
        conversation.fireTrigger = [this](const std::string& name, const google::protobuf::Struct& payload) {
            triggered = drive->fireTrigger(name, payload);
        };
        conversation.setSessionParam = [this](const std::string& name, const google::protobuf::Value& value) {
            drive->setSessionParam(name, value);
        };
        vehicle->setConversation(conversation);
        drive->start();
        drive->checkTriggers();   // the sensors' first values: taken as they are
        return true;
    }

    /** Says it, and returns the reply's text. */
    std::string say(const std::string& words) { return drive->handleText(words).text; }
    /** The car's state, at a path: at("climate", "ac"). */
    nlohmann::json at(const std::string& vertical, const std::string& field) const { return vehicle->read(vertical, field); }

    void expect(bool ok, const std::string& what) {
        if (ok) return;
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
    static bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
    int done(const char* name) const {
        std::printf("%s: %s\n", name, failed ? "failures" : "all passed");
        return failed ? 1 : 0;
    }

    std::shared_ptr<Car> vehicle;
    std::shared_ptr<const va::flow::CompiledAgent> agent;
    std::unique_ptr<va::flow::FlowRuntime> drive;
    va::flow::Reply triggered;   // what the last trigger a vertical fired said
    int failed = 0;
};

} // namespace car
