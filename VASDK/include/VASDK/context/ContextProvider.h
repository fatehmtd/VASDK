#ifndef CONTEXTPROVIDER_H
#define CONTEXTPROVIDER_H

#include <nlohmann/json.hpp>
#include <mutex>

namespace va {
    namespace context {
        /**
         * @brief Thread-safe key/value context store backed by JSON.
         */
        class ContextProvider {
            public:
            ContextProvider() = default;
            ContextProvider(const ContextProvider&) = delete;
            ContextProvider& operator=(const ContextProvider&) = delete;
            virtual ~ContextProvider() = default;

            virtual void setValue(const std::string& key, const nlohmann::json& value);
            virtual void removeValue(const std::string& key);
            virtual bool containsKey(const std::string& key) const;
            virtual nlohmann::json getValue(const std::string& key) const;
            virtual nlohmann::json getContext() const;

            protected:
            nlohmann::json _context;
            mutable std::mutex _mutex;
        };
    }
}


#endif