#include "GarageExtensions.h"

#include "garage.pb.h"

#include <VASDK/flow/CompiledAgent.h>

#include <cstdint>
#include <mutex>
#include <set>

namespace garage {

namespace {

// "AG-" and four digits from the booking's values (FNV-1a): stable across runs and platforms.
std::string reference(const std::string& booking) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : booking) hash = (hash ^ c) * 16777619u;
    const std::string digits = std::to_string(1000 + hash % 9000);
    return "AG-" + digits;
}

std::mutex bookingsMutex;
std::vector<nlohmann::json> bookings;   // shortcut: in memory; a real garage would post to its booking system

} // namespace

std::vector<nlohmann::json> savedBookings() {
    std::lock_guard<std::mutex> lock(bookingsMutex);
    return bookings;
}

void registerExtensions(va::flow::FlowRegistry& registry) {
    registry.addStep<garage::BookingReference>([](const garage::BookingReference& params, va::flow::TurnContext& turn) {
        std::string booking;
        for (const char* slot : {"$form.service", "$form.day", "$form.time", "$form.phone"})
            booking += va::flow::toText(turn.lookup(slot)) + "|";
        turn.set(params.into(), va::flow::stringValue(reference(booking)));
    });

    va::llm::Tool saveBooking("save_booking", "Keeps a confirmed booking");
    saveBooking.onCall([](const nlohmann::json& booking) -> nlohmann::json {
        if (!booking.contains("reference") || !booking["reference"].is_string() || booking["reference"].get<std::string>().empty())
            return {{"error", "a booking needs a reference"}};
        std::lock_guard<std::mutex> lock(bookingsMutex);
        bookings.push_back(booking);
        return {{"reference", booking["reference"]}, {"status", "confirmed"}};
    });
    registry.addTool(std::move(saveBooking));

    va::flow::FlowRegistry::Envelope thanks;
    thanks.before = [](va::flow::TurnContext& turn) {
        static const std::set<std::string> words{"thanks", "thank", "you", "very", "much", "cheers", "ok", "okay"};
        bool thanked = false;
        for (const auto& word : va::flow::splitWords(turn.text())) {
            if (!words.count(word)) return false;   // more than thanks: "no that's all thanks"
            thanked = thanked || word == "thanks" || word == "thank" || word == "cheers";
        }
        if (thanked) turn.say("You're welcome.");
        return thanked;
    };
    registry.addEnvelope("thanks", std::move(thanks));
}

} // namespace garage
