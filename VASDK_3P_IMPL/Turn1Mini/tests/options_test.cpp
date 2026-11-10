// turn-1-mini through the provider catalog: its options message (every field set, then none), the
// environment variables behind the thresholds, and the refusal of another message.
//
//   turn1mini_options_test <models dir>     exit 77 without <models dir>/turn_1_mini.onnx
#include "ProviderCatalog/ProviderCatalog.h"
#include "Turn1Mini/Turn1MiniProvider.h"

#include <VASDK/AudioRingBuffer.h>
#include <VASDK/engine/ProviderRegistry.h>
#include <vasdk/providers/options.pb.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

template <class Message>
std::shared_ptr<va::thirdparty::turn::Turn1MiniProvider> create(const va::engine::ProviderRegistry& registry,
                                                                 const va::engine::ProviderContext& context,
                                                                 const Message& options, std::string* error) {
    vasdk::engine::ProviderSpec spec;
    spec.set_id("turn-1-mini");
    spec.mutable_options()->PackFrom(options);
    return std::dynamic_pointer_cast<va::thirdparty::turn::Turn1MiniProvider>(
        registry.create<va::audio::TurnDetectionProvider>(spec, context, error));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: turn1mini_options_test <models dir>\n");
        return 2;
    }
    const std::string modelsDir = argv[1];
    if (!std::ifstream(modelsDir + "/turn_1_mini.onnx").good()) {
        std::printf("turn-1-mini: no model in %s, skipped\n", modelsDir.c_str());
        return 77;
    }
    setEnvironment("VASDK_TURN1MINI_THRESHOLD", nullptr);
    setEnvironment("VASDK_TURN1MINI_PAUSE_THRESHOLD", nullptr);

    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    va::engine::ProviderContext context;
    context.modelsDir = modelsDir;
    context.audio = {16000, va::audio::AudioFormat::SHORT, 1};
    context.frameSize = 1600;
    context.ringBuffer = std::make_shared<va::AudioRingBuffer>(16000 * 10, static_cast<int>(sizeof(short)));
    std::string error;

    // None set: the struct's defaults, the model from the models folder
    const va::thirdparty::turn::Turn1MiniOptions defaults;
    auto plain = create(registry, context, vasdk::providers::Turn1MiniOptions(), &error);
    check(plain != nullptr, "created with no option set " + error);
    if (plain) {
        const auto& options = plain->options();
        check(options.modelPath == modelsDir + "/turn_1_mini.onnx", "the model comes from the models folder");
        check(near(options.threshold, defaults.threshold) && near(options.pauseThreshold, defaults.pauseThreshold) &&
                  options.postAudioFrames == defaults.postAudioFrames && options.evaluateOnPause == defaults.evaluateOnPause,
              "every other option is the struct's default");
        check(near(plain->pauseThreshold(), defaults.pauseThreshold) && plain->evaluateOnPause() == defaults.evaluateOnPause,
              "the pause settings reach TurnDetectionProvider");
    }

    // Every field set
    vasdk::providers::Turn1MiniOptions every;
    every.set_model_path(modelsDir + "/turn_1_mini.onnx");
    every.set_threshold(0.42f);
    every.set_post_audio_frames(5);
    every.set_evaluate_on_pause(false);
    every.set_pause_threshold(0.77f);
    auto custom = create(registry, context, every, &error);
    check(custom != nullptr, "created with every option set " + error);
    if (custom) {
        const auto& options = custom->options();
        check(near(options.threshold, 0.42f) && near(options.pauseThreshold, 0.77f) && options.postAudioFrames == 5 &&
                  !options.evaluateOnPause,
              "every option reaches the provider");
        check(near(custom->pauseThreshold(), 0.77f) && !custom->evaluateOnPause(), "and TurnDetectionProvider");
    }

    // Unset thresholds come from the environment; a config value still wins
    setEnvironment("VASDK_TURN1MINI_THRESHOLD", "0.25");
    setEnvironment("VASDK_TURN1MINI_PAUSE_THRESHOLD", "0.6");
    auto fromEnvironment = create(registry, context, vasdk::providers::Turn1MiniOptions(), &error);
    check(fromEnvironment && near(fromEnvironment->options().threshold, 0.25f) && near(fromEnvironment->options().pauseThreshold, 0.6f),
          "unset thresholds come from VASDK_TURN1MINI_THRESHOLD and VASDK_TURN1MINI_PAUSE_THRESHOLD");
    auto configWins = create(registry, context, every, &error);
    check(configWins && near(configWins->options().threshold, 0.42f) && near(configWins->options().pauseThreshold, 0.77f),
          "a config value wins over the environment");
    setEnvironment("VASDK_TURN1MINI_THRESHOLD", nullptr);
    setEnvironment("VASDK_TURN1MINI_PAUSE_THRESHOLD", nullptr);

    // A missing model: no provider, so the spec's fallback starts
    vasdk::providers::Turn1MiniOptions missing;
    missing.set_model_path(modelsDir + "/no_such_model.onnx");
    check(create(registry, context, missing, &error) == nullptr, "a missing model gives no provider");

    // Another message is refused, naming its own
    auto refused = create(registry, context, vasdk::providers::SmartTurnOptions(), &error);
    check(!refused && error.find("vasdk.providers.Turn1MiniOptions") != std::string::npos,
          "another options message is refused, naming vasdk.providers.Turn1MiniOptions");

    std::printf("turn-1-mini options: %s\n", failures ? "failed" : "all passed");
    return failures ? 1 : 0;
}
