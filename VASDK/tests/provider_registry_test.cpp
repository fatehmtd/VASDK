// ProviderRegistry: create by id with typed options, default to the first registered, fall
// back when a provider is missing or fails to start (with the fallback's own options), reject options of the wrong type, and
// keep interfaces separate.

#include <VASDK/engine/ProviderRegistry.h>

#include <google/protobuf/wrappers.pb.h>

#include <cstdio>

using namespace va::engine;
using google::protobuf::StringValue;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

struct Speech { virtual ~Speech() = default; std::string name; };
struct Listener { virtual ~Listener() = default; };

std::shared_ptr<Speech> make(std::string name) {
    auto speech = std::make_shared<Speech>();
    speech->name = std::move(name);
    return speech;
}

vasdk::engine::ProviderSpec spec(const std::string& id, std::vector<std::string> fallback = {}) {
    vasdk::engine::ProviderSpec s;
    s.set_id(id);
    for (auto& f : fallback) s.add_fallback(f);
    return s;
}

} // namespace

int main() {
    ProviderRegistry registry;
    registry.add<Speech, StringValue>("voiced", [](const StringValue& options, const ProviderContext& context) {
        return make("voiced:" + (options.value().empty() ? std::string("default") : options.value()) + "@" + context.modelsDir);
    });
    registry.add<Speech>("plain", [](const ProviderContext&) { return make("plain"); });
    registry.add<Speech>("broken", [](const ProviderContext&) -> std::shared_ptr<Speech> { return nullptr; });
    registry.add<Listener>("ear", [](const ProviderContext&) { return std::make_shared<Listener>(); });

    ProviderContext context;
    context.modelsDir = "models";
    std::string error;

    auto withOptions = spec("voiced");
    StringValue voice;
    voice.set_value("jean");
    withOptions.mutable_options()->PackFrom(voice);
    auto created = registry.create<Speech>(withOptions, context, &error);
    check(created && created->name == "voiced:jean@models", "typed options and context reach the factory");

    created = registry.create<Speech>(spec(""), context);
    check(created && created->name == "voiced:default@models", "empty id: first registered, default options");

    created = registry.create<Speech>(spec("missing", {"plain"}), context);
    check(created && created->name == "plain", "fallback when the id is not built in");

    created = registry.create<Speech>(spec("broken", {"plain"}), context);
    check(created && created->name == "plain", "fallback when the provider fails to start");

    auto withFallbackOptions = spec("missing", {"voiced"});
    (*withFallbackOptions.mutable_fallback_options())["voiced"].PackFrom(voice);
    std::string chosen;
    created = registry.create<Speech>(withFallbackOptions, context, &error, &chosen);
    check(created && created->name == "voiced:jean@models" && chosen == "voiced", "a fallback gets its fallback_options; chosen names it");

    check(!registry.create<Speech>(spec("missing"), context, &error), "unknown id with no fallback fails");
    check(error.find("missing: not built into this binary") != std::string::npos, "unknown id reported");

    auto wrongType = spec("voiced");
    google::protobuf::Int32Value number;
    wrongType.mutable_options()->PackFrom(number);
    check(!registry.create<Speech>(wrongType, context, &error), "wrong options type fails");
    check(error.find("google.protobuf.StringValue") != std::string::npos, "wrong options type names the expected type");

    auto optionsForPlain = spec("plain");
    optionsForPlain.mutable_options()->PackFrom(voice);
    check(!registry.create<Speech>(optionsForPlain, context, &error) && error.find("takes no options") != std::string::npos,
          "options on a provider without options fail");

    // A provider with its own message that still takes an older, shared one, converted.
    registry.add<Speech, StringValue, google::protobuf::Int32Value>("versioned",
        [](const StringValue& options, const ProviderContext&) { return make("versioned:" + options.value()); },
        [](const google::protobuf::Int32Value& shared) {
            StringValue converted;
            converted.set_value("shared " + std::to_string(shared.value()));
            return converted;
        });
    auto own = spec("versioned");
    own.mutable_options()->PackFrom(voice);
    created = registry.create<Speech>(own, context, &error);
    check(created && created->name == "versioned:jean", "its own message reaches the factory");
    auto older = spec("versioned");
    google::protobuf::Int32Value seven;
    seven.set_value(7);
    older.mutable_options()->PackFrom(seven);
    created = registry.create<Speech>(older, context, &error);
    check(created && created->name == "versioned:shared 7", "the shared message is converted first");
    created = registry.create<Speech>(spec("versioned"), context, &error);
    check(created && created->name == "versioned:", "no options: its own message's defaults");
    auto neither = spec("versioned");
    neither.mutable_options()->PackFrom(google::protobuf::BoolValue());
    check(!registry.create<Speech>(neither, context, &error) && error.find("google.protobuf.StringValue") != std::string::npos
              && error.find("google.protobuf.Int32Value") != std::string::npos,
          "another type fails, naming both accepted types");

    auto ids = registry.ids<Speech>();
    check(ids == std::vector<std::string>({"voiced", "plain", "broken", "versioned"}), "ids in registration order");
    check(registry.ids<Listener>() == std::vector<std::string>({"ear"}), "interfaces are separate");
    check(registry.create<Listener>(spec(""), context) != nullptr, "second interface creates");

    ProviderRegistry empty;
    check(!empty.create<Speech>(spec(""), context, &error) && error == "no provider registered", "empty registry");

    if (failures == 0) std::printf("provider_registry_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
