#include "VASDK/TransportFactory.h"
#include "VASDK/llm/LLMTransport.h"
#include <spdlog/spdlog.h>

namespace va {
namespace transport {

TransportFactory::TransportFactory() {
    registerDefaults();
}

TransportFactory& TransportFactory::getInstance() {
    static TransportFactory instance;
    return instance;
}

void TransportFactory::registerTransport(const std::string& name, TransportCreator creator) {
    std::lock_guard<std::mutex> lock(_registryMutex);

    if (_registry.find(name) != _registry.end()) {
        spdlog::warn("[TransportFactory] Transport '{}' already registered, overwriting", name);
    }

    _registry[name] = creator;
    spdlog::info("[TransportFactory] Registered transport: {}", name);
}

std::shared_ptr<Transport> TransportFactory::createTransport(const std::string& name) {
    std::lock_guard<std::mutex> lock(_registryMutex);

    auto it = _registry.find(name);
    if (it == _registry.end()) {
        spdlog::error("[TransportFactory] Transport '{}' not found", name);
        return nullptr;
    }

    try {
        auto transport = it->second();
        spdlog::info("[TransportFactory] Created transport: {}", name);
        return transport;
    } catch (const std::exception& e) {
        spdlog::error("[TransportFactory] Failed to create transport '{}': {}", name, e.what());
        return nullptr;
    }
}

bool TransportFactory::isRegistered(const std::string& name) const {
    std::lock_guard<std::mutex> lock(_registryMutex);
    return _registry.find(name) != _registry.end();
}

std::vector<std::string> TransportFactory::getRegisteredTransports() const {
    std::lock_guard<std::mutex> lock(_registryMutex);

    std::vector<std::string> names;
    names.reserve(_registry.size());

    for (const auto& pair : _registry) {
        names.push_back(pair.first);
    }

    return names;
}

void TransportFactory::unregisterTransport(const std::string& name) {
    std::lock_guard<std::mutex> lock(_registryMutex);

    auto it = _registry.find(name);
    if (it != _registry.end()) {
        _registry.erase(it);
        spdlog::info("[TransportFactory] Unregistered transport: {}", name);
    }
}

void TransportFactory::clear() {
    std::lock_guard<std::mutex> lock(_registryMutex);
    _registry.clear();
    spdlog::info("[TransportFactory] Cleared all transport registrations");
}

void TransportFactory::registerDefaults() {
    // Register mock transport (always available)
    registerTransport("mock", []() {
        return std::make_shared<MockTransport>();
    });

    // Register WebSocket transport (stub)
    registerTransport("websocket", []() {
        return std::make_shared<WebSocketTransport>();
    });

    // LocalTransport needs audio source/sink params, so it isn't registered by default;
    // create it manually or register a custom creator.

    // LLM transports (OpenAITransport, AnthropicTransport) need API keys/config, so they
    // aren't registered by default; create them manually.

    spdlog::info("[TransportFactory] Registered default transports");
}

} // namespace transport
} // namespace va
