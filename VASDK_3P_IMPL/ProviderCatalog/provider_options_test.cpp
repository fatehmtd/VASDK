// Every provider takes its own options message (vasdk.providers.<Name>Options), and the providers
// that had a shared one (CloudOptions, LocalSttOptions, LocalTtsOptions) still take it; any other
// message is refused, naming the expected one. Every provider built into this binary is checked;
// none loads a model or opens a connection before it starts.
#include "ProviderCatalog/ProviderCatalog.h"

#include <VASDK/GenericAudioSource.h>
#include <VASDK/GenericTranscriptionProvider.h>
#include <VASDK/GenericVoiceSynthesisProvider.h>
#include <VASDK/QueuedVoiceSynthesisProvider.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <vasdk/providers/options.pb.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

namespace providers = vasdk::providers;
using google::protobuf::Any;

int failures = 0;
int checked = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::printf("FAIL %s\n", what.c_str());
    ++failures;
}

template <class Message>
Any packed(const Message& message) {
    Any any;
    any.PackFrom(message);
    return any;
}

struct Case {
    const char* id;
    Any own;                // its own message, with values that aren't the defaults
    Any shared;             // the shared message older configs used; empty: it never had one
    std::string ownType;    // the name the refusal must give
    std::function<void(const char*, void*)> after = nullptr;   // checks on the created provider
};

template <class Interface>
void checkStage(const va::engine::ProviderRegistry& registry, const char* stage, const std::vector<Case>& cases) {
    const auto built = registry.ids<Interface>();
    va::engine::ProviderContext context;
    context.modelsDir = "models";
    context.audio.sampleRate = 16000;
    context.frameSize = 1600;
    for (const auto& test : cases) {
        if (std::find(built.begin(), built.end(), test.id) == built.end()) continue;
        ++checked;
        const std::string name = std::string(stage) + " " + test.id;
        vasdk::engine::ProviderSpec spec;
        spec.set_id(test.id);
        std::string error;

        *spec.mutable_options() = test.own;
        auto provider = registry.create<Interface>(spec, context, &error);
        check(provider != nullptr, name + ": its own message: " + error);
        if (provider && test.after) test.after(test.id, provider.get());

        if (!test.shared.type_url().empty()) {
            *spec.mutable_options() = test.shared;
            check(registry.create<Interface>(spec, context, &error) != nullptr, name + ": the shared message: " + error);
        }

        *spec.mutable_options() = packed(providers::PyannoteOptions());
        check(!registry.create<Interface>(spec, context, &error) && error.find(test.ownType) != std::string::npos,
              name + ": another message is refused, naming " + test.ownType + " (got: " + error + ")");
    }
}

// The pre-buffer a local voice got from its own message.
std::function<void(const char*, void*)> prebufferIs(int expected) {
    return [expected](const char* id, void* provider) {
        auto* queued = dynamic_cast<va::audio::QueuedVoiceSynthesisProvider*>(static_cast<va::audio::GenericVoiceSynthesisProvider*>(provider));
        check(queued && queued->getPreBufferedSamplesMS() == expected,
              std::string("TTS ") + id + ": prebuffer_ms " + std::to_string(expected) + " reaches the voice");
    };
}

providers::CloudOptions cloudShared(const char* voice = "") {
    providers::CloudOptions shared;
    shared.set_api_key("test-key");
    shared.set_voice_id(voice);
    return shared;
}

} // namespace

int main() {
    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);

    providers::GradiumSttOptions gradiumStt;
    gradiumStt.set_api_key("test-key");
    gradiumStt.set_language("fr");
    providers::SonioxSttOptions sonioxStt;
    sonioxStt.set_api_key("test-key");
    sonioxStt.set_endpoint_detection(false);
    providers::DeepgramSttOptions deepgramStt;
    deepgramStt.set_api_key("test-key");
    deepgramStt.set_model("nova-3-medical");
    deepgramStt.set_endpointing_ms(500);
    deepgramStt.set_interim_results(false);
    providers::CartesiaSttOptions cartesiaStt;
    cartesiaStt.set_api_key("test-key");
    cartesiaStt.set_language("de");
    providers::FishAudioSttOptions fishaudioStt;
    fishaudioStt.set_api_key("test-key");
    fishaudioStt.set_language("es");
    providers::GladiaOptions gladia;
    gladia.set_api_key("test-key");
    gladia.set_region(providers::GladiaOptions::EU_WEST);
    gladia.set_endpointing_seconds(0.3);
    providers::WhistleOptions whistle;
    whistle.set_language("de");
    whistle.add_keywords("VASDK");
    whistle.set_word_timestamps(false);
    whistle.set_min_new_samples_for_partial(2000);
    providers::MoonshineOptions moonshine;
    moonshine.set_idle_timeout_ms(400);
    providers::ParakeetOptions parakeet;
    parakeet.set_incremental_partials(true);
    parakeet.set_high_confidence_shortcut_threshold(0.9f);
    parakeet.set_context_samples(4000);
    providers::LocalSttOptions localSttShared;
    localSttShared.set_idle_timeout_ms(600);

    checkStage<va::audio::GenericTranscriptionProvider>(registry, "STT", {
        {"gradium", packed(gradiumStt), packed(cloudShared()), "vasdk.providers.GradiumSttOptions"},
        {"soniox", packed(sonioxStt), packed(cloudShared()), "vasdk.providers.SonioxSttOptions"},
        {"deepgram", packed(deepgramStt), packed(cloudShared()), "vasdk.providers.DeepgramSttOptions"},
        {"cartesia", packed(cartesiaStt), packed(cloudShared()), "vasdk.providers.CartesiaSttOptions"},
        {"fishaudio", packed(fishaudioStt), packed(cloudShared()), "vasdk.providers.FishAudioSttOptions"},
        {"gladia", packed(gladia), packed(cloudShared()), "vasdk.providers.GladiaOptions"},
        {"whistle", packed(whistle), packed(localSttShared), "vasdk.providers.WhistleOptions"},
        {"moonshine", packed(moonshine), packed(localSttShared), "vasdk.providers.MoonshineOptions"},
        {"parakeet", packed(parakeet), packed(localSttShared), "vasdk.providers.ParakeetOptions"},
    });

    providers::GradiumTtsOptions gradiumTts;
    gradiumTts.set_api_key("test-key");
    gradiumTts.set_voice_id("voice");
    providers::SonioxTtsOptions sonioxTts;
    sonioxTts.set_api_key("test-key");
    sonioxTts.set_language("fr");
    providers::DeepgramTtsOptions deepgramTts;
    deepgramTts.set_api_key("test-key");
    deepgramTts.set_model("aura-2-orion-en");
    providers::CartesiaTtsOptions cartesiaTts;
    cartesiaTts.set_api_key("test-key");
    cartesiaTts.set_language("fr");
    providers::FishAudioTtsOptions fishaudioTts;
    fishaudioTts.set_api_key("test-key");
    fishaudioTts.set_model("s1-mini");
    providers::SupertonicOptions supertonic;
    supertonic.set_speed(1.2f);
    supertonic.set_prebuffer_ms(120);
    providers::MossOptions moss;
    moss.set_threads(2);
    moss.set_pause_short_seconds(0.2f);
    moss.set_prebuffer_ms(0);   // its own message can turn the pre-buffer off
    providers::PhononOptions phonon;
    phonon.set_temperature(0.5f);
    phonon.set_stream_chunk_frames(8);
    phonon.set_normalization_rules("all");
    phonon.set_prebuffer_ms(300);
    providers::PocketOptions pocket;
    pocket.set_temperature(0.5f);
    pocket.set_decoder_batch_frames(6);
    pocket.set_prebuffer_ms(150);
    providers::LocalTtsOptions localTtsShared;
    localTtsShared.set_voice("default");

    checkStage<va::audio::GenericVoiceSynthesisProvider>(registry, "TTS", {
        {"gradium", packed(gradiumTts), packed(cloudShared("voice")), "vasdk.providers.GradiumTtsOptions"},
        {"soniox", packed(sonioxTts), packed(cloudShared("Adrian")), "vasdk.providers.SonioxTtsOptions"},
        {"deepgram", packed(deepgramTts), packed(cloudShared("aura-2-thalia-en")), "vasdk.providers.DeepgramTtsOptions"},
        {"cartesia", packed(cartesiaTts), packed(cloudShared("voice")), "vasdk.providers.CartesiaTtsOptions"},
        {"fishaudio", packed(fishaudioTts), packed(cloudShared("voice")), "vasdk.providers.FishAudioTtsOptions"},
        {"supertonic", packed(supertonic), packed(localTtsShared), "vasdk.providers.SupertonicOptions", prebufferIs(120)},
        {"moss", packed(moss), packed(localTtsShared), "vasdk.providers.MossOptions", prebufferIs(0)},
        {"phonon", packed(phonon), packed(localTtsShared), "vasdk.providers.PhononOptions", prebufferIs(300)},
        {"pocket", packed(pocket), packed(localTtsShared), "vasdk.providers.PocketOptions", prebufferIs(150)},
    });

    providers::MiniAudioOptions miniaudio;
    miniaudio.set_buffer_seconds(10);
    checkStage<va::audio::GenericAudioSource>(registry, "audio input", {
        {"miniaudio", packed(miniaudio), Any(), "vasdk.providers.MiniAudioOptions"},
    });

    if (checked == 0) {
        std::printf("provider options: no provider built, skipped\n");
        return 77;
    }
    std::printf("provider options: %d provider(s), %s\n", checked, failures ? "failed" : "all passed");
    return failures ? 1 : 0;
}
