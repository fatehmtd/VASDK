#include "Car.h"

#include <algorithm>
#include <cctype>

namespace car {

//  Vertical

void Vertical::changed() { _car.changed(); }

nlohmann::json Vertical::control(const nlohmann::json& command) {
    auto all = tools();
    if (all.empty()) return error(name() + " has nothing to control");
    return all.front().execute(command);
}

nlohmann::json Vertical::simulate(const nlohmann::json&) { return error(name() + " has nothing to simulate"); }

std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string text(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "";
    return value.dump();
}

double number(const nlohmann::json& value, double fallback) {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        try { return std::stod(value.get<std::string>()); } catch (...) {}
    }
    return fallback;
}

std::string actionOf(const nlohmann::json& request, nlohmann::json& value, const std::vector<std::string>& valueNames) {
    value = request.value("value", nlohmann::json());
    if (request.contains("action")) return lower(text(request["action"]));
    std::vector<std::string> keys;
    for (const auto& [key, given] : request.items()) {
        const bool named = key == "value" || key == "zone" ||
                           std::find(valueNames.begin(), valueNames.end(), key) != valueNames.end();
        if (!named) keys.push_back(key);
    }
    if (keys.size() != 1) return "";
    if (value.is_null()) value = request[keys.front()];
    return lower(keys.front());
}

bool truthy(const nlohmann::json& value, bool fallback) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value.get<double>() != 0.0;
    if (value.is_string()) {
        const auto said = lower(value.get<std::string>());
        if (said == "on" || said == "true" || said == "yes" || said == "1") return true;
        if (said == "off" || said == "false" || said == "no" || said == "0") return false;
    }
    return fallback;
}

std::vector<std::string> words(const std::string& name) {
    std::vector<std::string> out;
    std::string word;
    for (const unsigned char c : name) {
        if (std::isalnum(c) || c == '\'') {
            word += static_cast<char>(std::tolower(c));
        } else if (!word.empty()) {
            out.push_back(word);
            word.clear();
        }
    }
    if (!word.empty()) out.push_back(word);
    return out;
}

//  Car

Vertical* Car::find(const std::string& name) const {
    for (const auto& vertical : _verticals)
        if (vertical->name() == name) return vertical.get();
    return nullptr;
}

std::vector<std::string> Car::names() const {
    std::vector<std::string> out;
    for (const auto& vertical : _verticals) out.push_back(vertical->name());
    return out;
}

nlohmann::json Car::state() const {
    nlohmann::json all = nlohmann::json::object();
    for (const auto& vertical : _verticals) all[vertical->name()] = vertical->state();
    return all;
}

nlohmann::json Car::read(const std::string& vertical, const std::string& field) const {
    const auto* found = find(vertical);
    if (!found) return nullptr;
    const auto state = found->state();
    return state.contains(field) ? state[field] : nlohmann::json();
}

std::vector<va::llm::Tool> Car::tools() const {
    std::vector<va::llm::Tool> all;
    for (const auto& vertical : _verticals)
        for (auto& tool : vertical->tools()) all.push_back(std::move(tool));
    return all;
}

void Car::registerWith(va::flow::FlowRegistry& registry) const {
    for (auto& tool : tools()) registry.addTool(std::move(tool));
    for (const auto& vertical : _verticals) vertical->registerExtensions(registry);
}

nlohmann::json Car::control(const nlohmann::json& command) {
    auto* vertical = find(command.value("vertical", std::string()));
    if (!vertical) return {{"error", "no vertical " + command.value("vertical", std::string())}};
    return vertical->control(command);
}

nlohmann::json Car::simulate(const nlohmann::json& command) {
    auto* vertical = find(command.value("vertical", std::string()));
    if (!vertical) return {{"error", "no vertical " + command.value("vertical", std::string())}};
    return vertical->simulate(command);
}

void Car::tick(double seconds) {
    for (const auto& vertical : _verticals) vertical->tick(seconds);
}

void Car::on(const std::string& signal, Handler handler) {
    std::lock_guard<std::mutex> lock(_mutex);
    _handlers[signal].push_back(std::move(handler));
}

void Car::emit(const std::string& signal, const nlohmann::json& payload) {
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _handlers.find(signal);
        if (found != _handlers.end()) handlers = found->second;
    }
    for (const auto& handler : handlers) handler(payload);
}

void Car::onChange(std::function<void(const nlohmann::json&)> listener) {
    std::lock_guard<std::mutex> lock(_mutex);
    _listeners.push_back(std::move(listener));
}

void Car::changed() {
    std::vector<std::function<void(const nlohmann::json&)>> listeners;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        listeners = _listeners;
    }
    if (listeners.empty()) return;
    const auto now = state();
    for (const auto& listener : listeners) listener(now);
}

void Car::setConversation(Conversation conversation) {
    std::lock_guard<std::mutex> lock(_mutex);
    _conversation = std::move(conversation);
}

Conversation Car::conversation() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _conversation;
}

} // namespace car
