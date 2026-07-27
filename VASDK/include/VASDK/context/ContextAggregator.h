#ifndef CONTEXTAGGREGATOR_H
#define CONTEXTAGGREGATOR_H

#include "ContextProvider.h"
#include <unordered_map>

namespace va::context
{
    /**
     * @brief Aggregates context from multiple named ContextProvider sources.
     */
    class ContextAggregator : public ContextProvider
    {
        public:
        ContextAggregator() = default;
        ~ContextAggregator() = default;

        virtual void setContextFromProvider(const std::string& name, const std::shared_ptr<ContextProvider>& provider);
        virtual void removeContextFromProvider(const std::string& name);
        virtual nlohmann::json getContext() const override;

        private:
        std::unordered_map<std::string, std::shared_ptr<ContextProvider>> _providerContexts;
    };
}

#endif