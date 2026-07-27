#pragma once

#include "Transport.h"
#include "LocalTransport.h"
#include "WebSocketTransport.h"
#include "MockTransport.h"
#include <map>
#include <functional>
#include <memory>
#include <string>

namespace va {
namespace transport {

/**
 * @brief Transport factory for creating transport instances
 *
 * The factory provides a registry-based system for creating transports
 * by name. This allows for runtime selection of transport types and
 * easy extensibility with custom transports.
 *
 * Example usage:
 * @code
 * auto factory = TransportFactory::getInstance();
 * auto transport = factory->createTransport("local");
 * @endcode
 */
class VASDK_EXPORT TransportFactory {
public:
    /**
     * @brief Transport creator function type
     */
    using TransportCreator = std::function<std::shared_ptr<Transport>()>;

    /**
     * @brief Get singleton instance
     * @return TransportFactory instance
     */
    static TransportFactory& getInstance();

    /**
     * @brief Register a transport creator
     * @param name Transport name (e.g., "local", "websocket")
     * @param creator Function that creates the transport
     */
    void registerTransport(const std::string& name, TransportCreator creator);

    /**
     * @brief Create a transport by name
     * @param name Transport name
     * @return Transport instance or nullptr if not found
     */
    std::shared_ptr<Transport> createTransport(const std::string& name);

    /**
     * @brief Check if a transport is registered
     * @param name Transport name
     * @return true if registered
     */
    bool isRegistered(const std::string& name) const;

    /**
     * @brief Get list of registered transport names
     * @return Vector of transport names
     */
    std::vector<std::string> getRegisteredTransports() const;

    /**
     * @brief Unregister a transport
     * @param name Transport name
     */
    void unregisterTransport(const std::string& name);

    /**
     * @brief Clear all registrations
     */
    void clear();

private:
    TransportFactory();
    ~TransportFactory() = default;

    // Prevent copying
    TransportFactory(const TransportFactory&) = delete;
    TransportFactory& operator=(const TransportFactory&) = delete;

    /**
     * @brief Register default transports
     */
    void registerDefaults();

    std::map<std::string, TransportCreator> _registry;
    mutable std::mutex _registryMutex;
};

/**
 * @brief Helper class for automatic transport registration
 *
 * Use this to register custom transports at static initialization time:
 *
 * @code
 * static TransportRegistrar<MyCustomTransport> myTransportRegistrar("mycustom");
 * @endcode
 */
template<typename T>
class TransportRegistrar {
public:
    TransportRegistrar(const std::string& name) {
        TransportFactory::getInstance().registerTransport(name, []() {
            return std::make_shared<T>();
        });
    }
};

} // namespace transport
} // namespace va
