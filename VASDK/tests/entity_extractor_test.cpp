// English values from speech, in either recognizer style (words or digits), on a fixed clock
// (Monday 2026-11-02, 09:00): numbers, phone numbers, dates, times, date-times, durations,
// emails, yes/no and ordinals; how replies say them; text made speakable for a plain voice; a
// DATE form slot read and said back; a choice picked by its place ("the second one").

#include <VASDK/flow/CompiledAgent.h>
#include <VASDK/flow/Entities.h>
#include <VASDK/flow/FlowRuntime.h>
#include <VASDK/nlu/Language.h>

#include <google/protobuf/text_format.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

using namespace va;
using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

std::chrono::system_clock::time_point localTime(int year, int month, int day, int hour) {
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

} // namespace

int main() {
    const auto now = localTime(2026, 11, 2, 9);
    const auto english = nlu::Language::builtIn();   // VASDK's en-US pack
    const EntityCatalog catalog(std::make_shared<const nlu::Languages>(nlu::Languages({english})));
    auto value = [&](const std::string& entity, const std::string& text) {
        const auto match = catalog.find(entity, text, {}, now);
        return match ? toText(match->value) : std::string("(none)");
    };
    auto expect = [&](const std::string& entity, const std::string& text, const std::string& wanted) {
        const auto got = value(entity, text);
        check(got == wanted, entity + " \"" + text + "\" → " + got + ", expected " + wanted);
    };

    // The table: the same value from words and from digits.
    expect("sys.number", "twenty three point five", "23.5");
    expect("sys.number", "23.5", "23.5");
    expect("sys.phone", "five five five, one two three, double four, six seven", "5551234467");
    expect("sys.phone", "555 123 4467", "5551234467");
    expect("sys.date", "next Tuesday", "2026-11-10");
    expect("sys.date", "Tuesday", "2026-11-03");
    expect("sys.date", "the fifth", "2026-11-05");
    expect("sys.date", "on the 1st", "2026-12-01");
    expect("sys.date", "in two weeks", "2026-11-16");
    expect("sys.date", "tomorrow", "2026-11-03");
    expect("sys.date", "November 20th", "2026-11-20");
    expect("sys.date", "the 3rd of January", "2027-01-03");
    expect("sys.date", "2026-12-24", "2026-12-24");
    expect("sys.time", "half past three in the afternoon", "15:30");
    expect("sys.time", "3:30 pm", "15:30");
    expect("sys.time", "at noon", "12:00");
    expect("sys.time", "quarter to five", "04:45");
    expect("sys.time", "nine oh five", "09:05");
    expect("sys.time", "at 9", "09:00");
    expect("sys.time", "seven thirty in the evening", "19:30");
    expect("sys.datetime", "tomorrow at noon", "2026-11-03T12:00");
    expect("sys.duration", "an hour and a half", "PT1H30M");
    expect("sys.duration", "45 minutes", "PT45M");
    expect("sys.duration", "half an hour", "PT30M");
    expect("sys.duration", "two days", "P2D");
    expect("sys.email", "john dot doe at gmail dot com", "john.doe@gmail.com");
    expect("sys.email", "it's John.Doe@Gmail.com thanks", "john.doe@gmail.com");
    expect("sys.yes_no", "yeah sure", "true");
    expect("sys.yes_no", "nope", "false");
    expect("sys.ordinal", "the second one", "2");
    expect("sys.ordinal", "number three", "3");
    expect("sys.ordinal", "the twenty first", "21");
    expect("sys.integer", "a hundred and two", "102");
    expect("sys.integer", "two thousand three hundred and five", "2305");
    expect("sys.integer", "nineteen", "19");
    expect("sys.time", "3pm", "15:00");
    expect("sys.time", "three o'clock", "03:00");
    expect("sys.date", "the 10 of November", "2026-11-10");
    expect("sys.yes_no", "That’s right, yes", "true");
    check(value("sys.time", "I have 3 cars") == "(none)", "a bare number isn't a time");
    check(value("sys.date", "I'm 5") == "(none)", "a bare number isn't a date");

    // How replies say them.
    check(english->spokenDate("2026-11-10") == "Tuesday, November 10th", "a date said");
    check(english->spokenDate("2026-11-22") == "Sunday, November 22nd", "22nd");
    check(english->spokenDate("2026-11-13") == "Friday, November 13th", "13th");
    check(english->spokenTime("15:30") == "3:30 PM" && english->spokenTime("09:00") == "9 AM" &&
              english->spokenTime("12:00") == "noon",
          "times said");
    check(english->spokenDuration("PT1H30M") == "1 hour and 30 minutes", "a duration said");
    check(english->spokenDuration("P1DT2H3M") == "1 day, 2 hours and 3 minutes", "three units");
    const auto speakable = english->speakable("Call 555-123-4567 on 2026-11-10 at 15:30, or at 4:00 PM.");
    check(speakable == "Call five five five, one two three, four five six seven on Tuesday, November 10th at 3:30 PM, or at 4:00 PM.",
          "speakable: " + speakable);

    // A DATE slot: read from the words, said back naturally; a choice by its place.
    vasdk::flow::Agent definition;
    google::protobuf::TextFormat::ParseFromString(R"agent(
        start_flow: "main"
        goals { name: "visit" slots { name: "day" type: DATE prompt: "Which day?" } }
        flows { name: "main" start_page: "visit"
          pages { name: "visit" form { goal: "visit" }
                  routes { condition: "$form.complete" steps { say { text: "See you $form.day." } } target: "pick" } }
          pages { name: "pick" on_enter { ask { say { text: "Oil or tires?" } choices { label: "Oil" } choices { label: "Tires" } } }
                  routes { condition: "$input.text = 'Tires'" steps { say { text: "Tires it is." } } } } }
    )agent", &definition);
    std::string error;
    auto agent = CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (agent) {
        FlowRuntime conversation(agent);
        conversation.setClock([now] { return now; });
        conversation.start();
        const auto reply = conversation.handleText("next tuesday please");
        check(reply.text.rfind("See you Tuesday, November 10th.", 0) == 0, "a DATE slot said back: " + reply.text);
        const auto picked = conversation.handleText("the second one");
        check(picked.text == "Tires it is. Oil or tires?", "a choice by its place: " + picked.text);
    }

    if (failures == 0) std::printf("entity_extractor_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
