// The C++ API from an install, as another project uses it: find_package(VASDK COMPONENTS cpp).
// A provider of the program's own, registered and then created from a config's ProviderSpec (a
// protobuf message) the way the engine does; and every installed provider, when the catalog is there.
#include <VASDK/VADProvider.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <VASDK/version.h>
#ifdef VASDK_CONSUMER_HAS_CATALOG
#include <ProviderCatalog/ProviderCatalog.h>
#endif

#include <cstdio>
#include <memory>
#include <string>

namespace {

class SilenceVad : public va::audio::VADProvider {
public:
    bool initialize(const va::audio::AudioConfig&, int) override { return true; }
    void release() override {}
    std::vector<VADResult> detectVoiceActivity(const char*, size_t, size_t) const override { return {}; }
};

}  // namespace

int main() {
    va::engine::ProviderRegistry registry;
    registry.add<va::audio::VADProvider>("silence", [](const va::engine::ProviderContext&) {
        return std::make_shared<SilenceVad>();
    });
#ifdef VASDK_CONSUMER_HAS_CATALOG
    va::thirdparty::registerAllProviders(registry);
#endif

    vasdk::engine::ProviderSpec spec;
    spec.set_id("silence");
    std::string error;
    std::string chosen;
    const auto vad = registry.create<va::audio::VADProvider>(spec, va::engine::ProviderContext{}, &error, &chosen);
    if (!vad || chosen != "silence") {
        std::fprintf(stderr, "no VAD from the spec: %s\n", error.c_str());
        return 1;
    }
    const auto vads = registry.ids<va::audio::VADProvider>();
    std::printf("VASDK %s, C++ API: %s created, %zu VAD provider(s) registered\n", VASDK_VERSION_STRING,
                vad->getName().c_str(), vads.size());
    return 0;
}
