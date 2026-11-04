#include "CallCenterTools.h"

#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace callcenter {

namespace {

struct Account {
    std::string id, name, phone, tier;
};

// shortcut: a fixed table; a real call center asks its CRM here (an HTTP call, a database query).
const std::vector<Account>& accounts() {
    static const std::vector<Account> table{
        {"ACC-001", "Alice Johnson", "555-1234", "premium"},
        {"ACC-002", "Bob Smith", "555-5678", "standard"},
    };
    return table;
}

std::string digitsOf(const std::string& text) {
    std::string digits;
    for (const unsigned char c : text)
        if (std::isdigit(c)) digits += static_cast<char>(c);
    return digits;
}

std::string upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// By phone number, however it's written, or by account id ("acc 001", "ACC-001").
std::optional<Account> find(const std::string& identifier) {
    const auto digits = digitsOf(identifier);
    for (const auto& account : accounts()) {
        if (!digits.empty() && digits == digitsOf(account.phone)) return account;
        if (upper(identifier).find("ACC") != std::string::npos && !digits.empty() && digits == digitsOf(account.id)) return account;
    }
    return std::nullopt;
}

// "CB-" and four digits from the callback's values (FNV-1a): the same for the same callback.
std::string reference(const nlohmann::json& callback) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char c : callback.dump()) hash = (hash ^ c) * 16777619u;
    return "CB-" + std::to_string(1000 + hash % 9000);
}

std::mutex callbacksMutex;
std::vector<nlohmann::json> callbacks;   // shortcut: in memory; a real call center posts to its scheduling system

} // namespace

std::vector<nlohmann::json> scheduledCallbacks() {
    std::lock_guard<std::mutex> lock(callbacksMutex);
    return callbacks;
}

void registerTools(va::flow::FlowRegistry& registry) {
    va::llm::Tool lookup("lookup_account", "Looks up a customer's account by phone number or account id");
    lookup.addParameter({"identifier", "phone number or account id", "string", true});
    lookup.onCall([](const nlohmann::json& args) -> nlohmann::json {
        const auto identifier = args.value("identifier", std::string());
        const auto account = find(identifier);
        if (!account) return {{"found", false}};
        return {{"found", true}, {"account_id", account->id}, {"name", account->name},
                {"phone", account->phone}, {"tier", account->tier}};
    });
    registry.addTool(std::move(lookup));

    va::llm::Tool schedule("schedule_callback", "Schedules a service callback");
    schedule.onCall([](const nlohmann::json& callback) -> nlohmann::json {
        for (const char* required : {"customer_name", "phone_number", "service_type", "appointment_date"})
            if (!callback.contains(required) || !callback[required].is_string() || callback[required].get<std::string>().empty())
                return {{"error", std::string("a callback needs ") + required}};
        std::lock_guard<std::mutex> lock(callbacksMutex);
        callbacks.push_back(callback);
        return {{"reference", reference(callback)}, {"status", "scheduled"}};
    });
    registry.addTool(std::move(schedule));
}

} // namespace callcenter
