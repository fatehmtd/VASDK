#include "VASDK/context/ContextProvider.h"

namespace va {
    namespace context {
        void ContextProvider::setValue(const std::string& key, const nlohmann::json& value) {
            std::lock_guard<std::mutex> lock(_mutex);
            _context[key] = value;
        }

        void ContextProvider::removeValue(const std::string& key) {
            std::lock_guard<std::mutex> lock(_mutex);
            _context.erase(key);
        }

        bool ContextProvider::containsKey(const std::string& key) const {
            std::lock_guard<std::mutex> lock(_mutex);
            return _context.contains(key);
        }

        nlohmann::json ContextProvider::getValue(const std::string& key) const {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _context.find(key);
            if (it != _context.end()) {
                return it.value();
            }
            return nullptr;
        }

        nlohmann::json ContextProvider::getContext() const {
            std::lock_guard<std::mutex> lock(_mutex);
            return _context;
        }
    }
}
