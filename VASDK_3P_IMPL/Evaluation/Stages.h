#pragma once

// One evaluation per stage (docs/evaluation.md). Each creates its provider through the catalog by
// id, as a session does, runs it on the evaluation set's clips and adds what it measured to the
// report. A stage is skipped, never failed, when its provider isn't built or doesn't start (its
// model missing).

#include "EvalData.h"
#include "Report.h"

#include <VASDK/engine/ProviderRegistry.h>
#include "vasdk/engine/engine_config.pb.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace va::audio { class GenericTranscriptionProvider; }

namespace va::thirdparty::evaluation {

struct StageContext {
    va::engine::ProviderRegistry* registry = nullptr;
    va::engine::ProviderContext context;   // 16 kHz 16-bit mono, 100 ms frames, the models folder
    const EvalData* data = nullptr;
    vasdk::eval::Tier tier = vasdk::eval::FULL;
    std::string outputFolder;              // where synthesized speech is written; empty: nowhere
    std::string judge;                     // the STT that transcribes synthesized and denoised speech
    bool realtime = false;                 // streaming STT fed at the pace of speech (latencies mean something)
    bool verbose = false;
};

struct StageResult {
    bool ran = false;
    std::string reason;   // why it was skipped
};

StageResult evaluateStt(const StageContext& stage, const std::string& provider, const std::string& mode, Report& report);
StageResult evaluateTts(const StageContext& stage, const std::string& provider, Report& report);
StageResult evaluateVad(const StageContext& stage, const std::string& provider, Report& report);
StageResult evaluateTurn(const StageContext& stage, const std::string& provider, Report& report);
StageResult evaluateSpeaker(const StageContext& stage, const std::string& provider, Report& report);
StageResult evaluateDenoise(const StageContext& stage, const std::string& provider, Report& report);

/// The local providers of a stage this build has, in the catalog's order: cloud ones are left out
std::vector<std::string> localProviders(const va::engine::ProviderRegistry& registry, const std::string& stage);

/// The judge: `wanted` when built, else the most accurate local STT built (parakeet, whistle, moonshine)
std::string judgeFor(const va::engine::ProviderRegistry& registry, const std::string& wanted);

/// A provider of the catalog by id, as a session creates it
template <class Interface>
std::shared_ptr<Interface> createProvider(const StageContext& stage, const std::string& id, std::string* reason) {
    vasdk::engine::ProviderSpec spec;
    spec.set_id(id);
    return stage.registry->create<Interface>(spec, stage.context, reason);
}

template <class Interface>
bool isBuilt(const va::engine::ProviderRegistry& registry, const std::string& id) {
    const auto ids = registry.ids<Interface>();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// What a started STT hears in 16 kHz 16-bit mono audio, in one call; empty when it throws (error says why)
std::string transcribeOnce(va::audio::GenericTranscriptionProvider& recognizer, const std::vector<short>& pcm, std::string* error);

/// Seconds since an arbitrary start, for timings
double now();

} // namespace va::thirdparty::evaluation
