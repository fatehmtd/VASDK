// A cloud provider with no API key gives no provider, so the registry starts the spec's next
// fallback; with its key, it's created (nothing is sent until it starts). Every cloud STT and TTS
// built into this binary is checked.
#include "ProviderCatalog/ProviderCatalog.h"

#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/engine/ProviderRegistry.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct CloudProvider {
    const char* id;
    const char* keyVariable;
};

constexpr CloudProvider kCloudProviders[] = {
    {"gradium", "GRADIUM_API_KEY"}, {"soniox", "SONIOX_API_KEY"},       {"deepgram", "DEEPGRAM_API_KEY"},
    {"cartesia", "CARTESIA_API_KEY"}, {"fishaudio", "FISHAUDIO_API_KEY"}, {"gladia", "GLADIA_API_KEY"},
};

void setVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

template <class Interface>
int checkStage(const va::engine::ProviderRegistry& registry, const char* stage) {
    int failures = 0, checked = 0;
    const auto built = registry.ids<Interface>();
    va::engine::ProviderContext context;
    for (const auto& provider : kCloudProviders) {
        if (std::find(built.begin(), built.end(), provider.id) == built.end()) continue;
        ++checked;
        vasdk::engine::ProviderSpec spec;
        spec.set_id(provider.id);

        setVariable(provider.keyVariable, nullptr);
        std::string error;
        if (registry.create<Interface>(spec, context, &error)) {
            std::printf("FAIL %s %s: created with no key\n", stage, provider.id);
            ++failures;
        } else if (error.find(provider.id) == std::string::npos) {
            std::printf("FAIL %s %s: the error doesn't name it: %s\n", stage, provider.id, error.c_str());
            ++failures;
        }

        setVariable(provider.keyVariable, "test-key");
        if (!registry.create<Interface>(spec, context)) {
            std::printf("FAIL %s %s: not created with its key\n", stage, provider.id);
            ++failures;
        }
        setVariable(provider.keyVariable, nullptr);
    }
    std::printf("%s: %d cloud provider(s) checked\n", stage, checked);
    return failures;
}

} // namespace

int main() {
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    const int failures = checkStage<va::audio::GenericTranscriptionProvider>(registry, "STT")
                       + checkStage<va::audio::GenericVoiceSynthesisProvider>(registry, "TTS");
    std::printf("cloud keys: %s\n", failures ? "failed" : "all passed");
    return failures ? 1 : 0;
}
