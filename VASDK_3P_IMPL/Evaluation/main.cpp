// vasdk_provider_eval: the local providers measured on real speech (docs/evaluation.md).
//
//   vasdk_provider_eval <stt|tts|vad|turn|speaker|denoise|all> [options]
//     --provider <id>            one provider of the stage (default: every local one built)
//     --mode <oneshot|streaming|both>   STT (default: both)
//     --tier <smoke|full>        the clips (default: full)
//     --models <dir>             the models folder (default: VASDK_MODELS_DIR, else the model store's)
//     --data <dir>               the evaluation set, the folder of references.txtpb
//     --store <dir>              else: download the set into this folder first (SHA-256 checked)
//     --manifest <file>          the set's manifest for --store (default: the one in the source tree)
//     --judge <stt id>           the STT that judges TTS and denoising (default: parakeet, whistle, moonshine)
//     --report <dir>             write report.md, report.json and the synthesized speech there
//     --check <baselines.txtpb>  fail when a gated score is worse than its baseline + margin
//     --write-baselines <file>   put this run's gated scores in that file
//     --realtime                 feed streaming STT at the pace of speech (latencies mean something)
//     --verbose                  a line per clip
//     --log-level <level>        the providers' logs: trace, debug, info, warn (default), error, off
//
// Exit: 0 measured (and within baselines), 1 a score past its baseline or an error, 77 nothing ran
// (no provider built, a model missing, no data): ctest counts that as skipped.

#include "EvalData.h"
#include "Report.h"
#include "Stages.h"

#include "ProviderCatalog/ProviderCatalog.h"

#include <VASDK/ModelStore.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef VASDK_EVAL_MANIFEST
#define VASDK_EVAL_MANIFEST ""
#endif

using namespace va::thirdparty::evaluation;

namespace {

constexpr int kSkipped = 77;
constexpr const char* kStages[] = {"stt", "tts", "vad", "turn", "speaker", "denoise"};

struct Arguments {
    std::string stage, provider, mode = "both", tier = "full", models, data, store, manifest = VASDK_EVAL_MANIFEST, judge, report, check,
                                                 writeBaselines, logLevel = "warn";
    bool realtime = false, verbose = false;
};

int usage() {
    std::fprintf(stderr,
                 "usage: vasdk_provider_eval <stt|tts|vad|turn|speaker|denoise|all> [--provider id] [--mode oneshot|streaming|both]\n"
                 "       [--tier smoke|full] [--models dir] [--data dir | --store dir [--manifest file]] [--judge stt-id]\n"
                 "       [--report dir] [--check baselines.txtpb] [--write-baselines file] [--realtime] [--verbose]\n");
    return 2;
}

bool parse(int argc, char** argv, Arguments& arguments) {
    if (argc < 2) return false;
    arguments.stage = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        auto value = [&](std::string& into) {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        if (flag == "--provider" ? !value(arguments.provider)
            : flag == "--mode" ? !value(arguments.mode)
            : flag == "--tier" ? !value(arguments.tier)
            : flag == "--models" ? !value(arguments.models)
            : flag == "--data" ? !value(arguments.data)
            : flag == "--store" ? !value(arguments.store)
            : flag == "--manifest" ? !value(arguments.manifest)
            : flag == "--judge" ? !value(arguments.judge)
            : flag == "--report" ? !value(arguments.report)
            : flag == "--check" ? !value(arguments.check)
            : flag == "--write-baselines" ? !value(arguments.writeBaselines)
            : flag == "--log-level" ? !value(arguments.logLevel)
            : flag == "--realtime" ? !(arguments.realtime = true)
            : flag == "--verbose" ? !(arguments.verbose = true)
            : true) {
            std::fprintf(stderr, "unknown or incomplete option %s\n", flag.c_str());
            return false;
        }
    }
    const bool knownStage = arguments.stage == "all" || std::find(std::begin(kStages), std::end(kStages), arguments.stage) != std::end(kStages);
    return knownStage && (arguments.tier == "smoke" || arguments.tier == "full") &&
           (arguments.mode == "oneshot" || arguments.mode == "streaming" || arguments.mode == "both");
}

std::string modelsFolder(const std::string& given) {
    if (!given.empty()) return given;
    if (const char* environment = std::getenv("VASDK_MODELS_DIR"); environment && *environment) return environment;
    return va::ModelStore::defaultDirectory();
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parse(argc, argv, arguments)) return usage();
    spdlog::set_level(spdlog::level::from_str(arguments.logLevel));

    std::string error;
    const std::string folder = locateData(arguments.data, arguments.store, arguments.manifest, &error);
    if (folder.empty()) {
        std::fprintf(stderr, "skipped: %s\n", error.c_str());
        return kSkipped;
    }
    EvalData data;
    if (!data.load(folder, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    va::engine::ProviderRegistry registry;
    va::thirdparty::registerAllProviders(registry);
    StageContext stage;
    stage.registry = &registry;
    stage.context.modelsDir = std::filesystem::absolute(modelsFolder(arguments.models)).string();
    stage.context.audio = va::audio::AudioConfig{kSampleRate, va::audio::AudioFormat::SHORT, 1};
    stage.context.frameSize = kSampleRate / 10;
    stage.data = &data;
    stage.tier = arguments.tier == "smoke" ? vasdk::eval::SMOKE : vasdk::eval::FULL;
    stage.outputFolder = arguments.report;
    stage.realtime = arguments.realtime;
    stage.verbose = arguments.verbose;
    if (!arguments.report.empty()) std::filesystem::create_directories(arguments.report);

    Report report;
    int ran = 0;
    std::vector<std::string> skipped;
    for (const char* name : kStages) {
        const std::string stageName = name;
        if (arguments.stage != "all" && arguments.stage != stageName) continue;
        if ((stageName == "tts" || stageName == "denoise") && stage.judge.empty()) {
            stage.judge = judgeFor(registry, arguments.judge);
        }
        const std::vector<std::string> providers =
            arguments.provider.empty() ? localProviders(registry, stageName) : std::vector<std::string>{arguments.provider};
        if (providers.empty()) skipped.push_back(stageName + ": no local provider built");
        for (const auto& provider : providers) {
            std::vector<StageResult> results;
            if (stageName == "stt") {
                for (const char* mode : {"oneshot", "streaming"})
                    if (arguments.mode == "both" || arguments.mode == mode) results.push_back(evaluateStt(stage, provider, mode, report));
            } else if (stageName == "tts") {
                results.push_back(evaluateTts(stage, provider, report));
            } else if (stageName == "vad") {
                results.push_back(evaluateVad(stage, provider, report));
            } else if (stageName == "turn") {
                results.push_back(evaluateTurn(stage, provider, report));
            } else if (stageName == "speaker") {
                results.push_back(evaluateSpeaker(stage, provider, report));
            } else {
                results.push_back(evaluateDenoise(stage, provider, report));
            }
            for (const auto& result : results) {
                if (result.ran) ++ran;
                else skipped.push_back(result.reason);
            }
        }
    }

    report.print();
    for (const auto& reason : skipped) std::printf("skipped: %s\n", reason.c_str());
    if (!arguments.report.empty()) {
        const auto path = std::filesystem::path(arguments.report);
        report.writeMarkdown((path / "report.md").string(), "VASDK provider evaluation (" + arguments.tier + " tier)");
        report.writeJson((path / "report.json").string());
        std::printf("report: %s\n", (path / "report.md").string().c_str());
    }
    if (ran == 0) return kSkipped;

    int status = 0;
    if (!arguments.check.empty()) {
        vasdk::eval::Baselines baselines;
        if (!loadBaselines(arguments.check, baselines, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const auto result = checkBaselines(report.scores(), baselines, stage.tier);
        for (const auto& line : result.unchecked) std::printf("no baseline: %s\n", line.c_str());
        for (const auto& line : result.failures) std::printf("WORSE THAN BASELINE: %s\n", line.c_str());
        std::printf("%d score(s) checked against %s, %zu worse\n", result.checked, arguments.check.c_str(), result.failures.size());
        if (!result.failures.empty()) status = 1;
    }
    if (!arguments.writeBaselines.empty()) {
        vasdk::eval::Baselines baselines;
        if (std::filesystem::exists(arguments.writeBaselines) && !loadBaselines(arguments.writeBaselines, baselines, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        updateBaselines(baselines, report.scores(), stage.tier);
        if (!saveBaselines(arguments.writeBaselines, baselines, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("baselines written to %s\n", arguments.writeBaselines.c_str());
    }
    return status;
}
