#include "VASDK/context/ContextAggregator.h"

namespace va::context
{
    void ContextAggregator::setContextFromProvider(const std::string& name,
        const std::shared_ptr<ContextProvider>& provider)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _providerContexts[name] = provider;
    }

    void ContextAggregator::removeContextFromProvider(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _providerContexts.erase(name);
    }

    nlohmann::json ContextAggregator::getContext() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        nlohmann::json aggregated = ContextProvider::getContext();
        for (const auto& [name, provider] : _providerContexts) {
            if (provider) {
                aggregated[name] = provider->getContext();
            }
        }
        return aggregated;
    }
}
