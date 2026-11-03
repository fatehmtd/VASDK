#pragma once

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/CommonAudioUtils.h>
#include <vasdk/engine/engine_config.pb.h>

#include <spdlog/spdlog.h>

#include <any>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

namespace va {
namespace engine {

/** What every provider factory receives besides its own options. */
struct ProviderContext {
    std::string modelsDir;
    audio::AudioConfig audio{};
    int frameSize = 0;   // samples per pipeline frame
    std::shared_ptr<AudioRingBuffer> ringBuffer;   // mic audio, for providers that read it directly
};

/**
 * Maps provider ids ("parakeet", "soniox", ...) to factories, per provider interface
 * (GenericTranscriptionProvider, GenericVoiceSynthesisProvider, VADProvider, ...).
 * Fill it once at startup, then create providers from a vasdk.engine.ProviderSpec.
 * Not thread-safe while providers are being added.
 */
class ProviderRegistry {
public:
    /** Factory with a typed options message; the spec's `options` must hold an `Options`. */
    template <class Interface, class Options>
    void add(const std::string& id,
             std::function<std::shared_ptr<Interface>(const Options&, const ProviderContext&)> factory) {
        entries<Interface>().push_back({id,
            [id, factory](const google::protobuf::Any& any, const ProviderContext& context,
                          std::string& error) -> std::shared_ptr<Interface> {
                Options options;
                if (!any.type_url().empty() && (!any.template Is<Options>() || !any.UnpackTo(&options))) {
                    error = id + ": options must be " + std::string(Options::descriptor()->full_name())
                            + ", got " + any.type_url();
                    return nullptr;
                }
                return factory(options, context);
            }});
    }

    /** Factory without options; a spec that sets `options` for it is an error. */
    template <class Interface>
    void add(const std::string& id,
             std::function<std::shared_ptr<Interface>(const ProviderContext&)> factory) {
        entries<Interface>().push_back({id,
            [id, factory](const google::protobuf::Any& any, const ProviderContext& context,
                          std::string& error) -> std::shared_ptr<Interface> {
                if (!any.type_url().empty()) { error = id + ": takes no options"; return nullptr; }
                return factory(context);
            }});
    }

    /**
     * Creates `spec.id` (the first registered provider when empty), then each `spec.fallback`
     * in order until one starts. Options apply to `spec.id` only; fallbacks get their defaults.
     * Returns nullptr with `error` listing why each candidate was skipped.
     */
    template <class Interface>
    std::shared_ptr<Interface> create(const vasdk::engine::ProviderSpec& spec, const ProviderContext& context,
                                      std::string* error = nullptr) const {
        const auto* registered = find<Interface>();
        std::vector<std::string> candidates;
        if (!spec.id().empty()) candidates.push_back(spec.id());
        else if (registered && !registered->empty()) candidates.push_back(registered->front().id);
        candidates.insert(candidates.end(), spec.fallback().begin(), spec.fallback().end());

        std::string reasons;
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::string reason;
            const Entry<Interface>* entry = nullptr;
            if (registered)
                for (const auto& e : *registered) if (e.id == candidates[i]) { entry = &e; break; }
            if (!entry) {
                reason = candidates[i] + ": not built into this binary";
            } else if (auto provider = entry->create(i == 0 ? spec.options() : google::protobuf::Any(),
                                                     context, reason)) {
                if (i > 0) spdlog::warn("[ProviderRegistry] using fallback '{}' ({})", candidates[i], reasons);
                else spdlog::info("[ProviderRegistry] using '{}'", candidates[i]);
                return provider;
            } else if (reason.empty()) {
                reason = candidates[i] + ": failed to start";
            }
            spdlog::warn("[ProviderRegistry] {}", reason);
            reasons += (reasons.empty() ? "" : "; ") + reason;
        }
        if (error) *error = candidates.empty() ? "no provider registered" : reasons;
        return nullptr;
    }

    /** Registered ids in registration order. */
    template <class Interface>
    std::vector<std::string> ids() const {
        std::vector<std::string> result;
        if (const auto* registered = find<Interface>())
            for (const auto& e : *registered) result.push_back(e.id);
        return result;
    }

private:
    template <class Interface>
    struct Entry {
        std::string id;
        std::function<std::shared_ptr<Interface>(const google::protobuf::Any&, const ProviderContext&,
                                                 std::string&)> create;
    };

    template <class Interface>
    std::vector<Entry<Interface>>& entries() {
        auto& slot = _entries[std::type_index(typeid(Interface))];
        if (!slot.has_value()) slot = std::vector<Entry<Interface>>();
        return std::any_cast<std::vector<Entry<Interface>>&>(slot);
    }

    template <class Interface>
    const std::vector<Entry<Interface>>* find() const {
        auto it = _entries.find(std::type_index(typeid(Interface)));
        return it == _entries.end() ? nullptr : std::any_cast<std::vector<Entry<Interface>>>(&it->second);
    }

    std::map<std::type_index, std::any> _entries;
};

} // namespace engine
} // namespace va
