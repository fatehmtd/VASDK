// The pre-buffer a local voice gets from the catalog: its own prebuffer_ms, else the engine's
// audio.tts_prebuffer_ms (ProviderContext::ttsPrebufferMs), else its options struct's 250 ms.
// Every local TTS built into this binary is checked; none loads a model before it starts.
#include "ProviderCatalog/ProviderCatalog.h"

#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <vasdk/providers/options.pb.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace {

constexpr const char* kLocalVoices[] = {"pocket", "moss", "phonon", "supertonic"};

// The pre-buffer of `id` created with these settings; -1 when it isn't created or isn't a queued voice.
int prebufferOf(const va::engine::ProviderRegistry& registry, const char* id, int ownMs, int engineMs) {
    vasdk::engine::ProviderSpec spec;
    spec.set_id(id);
    if (ownMs > 0) {
        vasdk::providers::LocalTtsOptions options;
        options.set_prebuffer_ms(ownMs);
        spec.mutable_options()->PackFrom(options);
    }
    va::engine::ProviderContext context;
    context.audio.sampleRate = 16000;
    context.ttsPrebufferMs = engineMs;
    std::string error;
    const auto provider = registry.create<va::audio::GenericVoiceSynthesisProvider>(spec, context, &error);
    const auto queued = std::dynamic_pointer_cast<va::audio::QueuedVoiceSynthesisProvider>(provider);
    if (!queued) std::printf("  %s not created: %s\n", id, error.c_str());
    return queued ? queued->getPreBufferedSamplesMS() : -1;
}

} // namespace

int main() {
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    const auto built = registry.ids<va::audio::GenericVoiceSynthesisProvider>();

    struct Case { int ownMs, engineMs, expected; const char* what; };
    constexpr Case kCases[] = {
        {0, -1, 250, "nothing set: the voice's default"},
        {0, 400, 400, "audio.tts_prebuffer_ms"},
        {0, 0, 0, "audio.tts_prebuffer_ms 0 plays at once"},
        {100, 400, 100, "the voice's own prebuffer_ms wins"},
    };

    int failures = 0, checked = 0;
    for (const char* id : kLocalVoices) {
        if (std::find(built.begin(), built.end(), id) == built.end()) continue;
        ++checked;
        for (const auto& test : kCases) {
            const int got = prebufferOf(registry, id, test.ownMs, test.engineMs);
            if (got == test.expected) continue;
            std::printf("FAIL %s, %s: %d ms, expected %d\n", id, test.what, got, test.expected);
            ++failures;
        }
    }
    if (checked == 0) {
        std::printf("tts prebuffer: no local TTS built, skipped\n");
        return 77;
    }
    std::printf("tts prebuffer: %d local voice(s), %s\n", checked, failures ? "failed" : "all passed");
    return failures ? 1 : 0;
}
