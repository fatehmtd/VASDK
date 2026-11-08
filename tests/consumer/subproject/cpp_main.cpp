// The C++ API from a project that adds VASDK from source: the providers built in, by config id.
#include <ProviderCatalog/ProviderCatalog.h>
#include <VASDK/VADProvider.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <VASDK/version.h>

#include <cstdio>

int main() {
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    const auto vads = registry.ids<va::audio::VADProvider>();
    std::printf("VASDK %s: %zu VAD provider(s)\n", VASDK_VERSION_STRING, vads.size());
    return vads.empty() ? 1 : 0;   // the default flavor has Silero and energy/ZCR
}
