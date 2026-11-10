#include "Report.h"

#include <google/protobuf/text_format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace va::thirdparty::evaluation {

namespace {

bool fail(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

std::string key(const std::string& stage, const std::string& provider, const std::string& mode, const std::string& language,
                const std::string& metric) {
    return stage + "/" + provider + (mode.empty() ? "" : "/" + mode) + (language.empty() ? "" : "/" + language) + " " + metric;
}

std::string key(const Score& score) { return key(score.stage, score.provider, score.mode, score.language, score.metric); }

const char* unitName(Unit unit) {
    switch (unit) {
    case Unit::Rate: return "rate";
    case Unit::Seconds: return "seconds";
    case Unit::Ratio: return "ratio";
    case Unit::Count: return "count";
    }
    return "rate";
}

// The stages in the order the report shows them
constexpr const char* kStageOrder[] = {"stt", "tts", "vad", "turn", "speaker", "denoise"};

int stageRank(const std::string& stage) {
    for (size_t i = 0; i < std::size(kStageOrder); ++i)
        if (stage == kStageOrder[i]) return static_cast<int>(i);
    return static_cast<int>(std::size(kStageOrder));
}

} // namespace

std::string formatValue(double value, Unit unit) {
    char text[64];
    switch (unit) {
    case Unit::Rate: std::snprintf(text, sizeof(text), "%.1f%%", 100 * value); break;
    case Unit::Seconds: std::snprintf(text, sizeof(text), "%.3f s", value); break;
    case Unit::Ratio: std::snprintf(text, sizeof(text), "%.3f", value); break;
    case Unit::Count: std::snprintf(text, sizeof(text), "%.0f", value); break;
    }
    return text;
}

void Report::note(const std::string& stage, const std::string& provider, const std::string& line) {
    _notes.push_back({stage, provider, line});
}

bool Report::writeMarkdown(const std::string& path, const std::string& title) const {
    std::ostringstream out;
    out << "# " << title << "\n\n";
    std::vector<std::string> stages;
    for (const auto& score : _scores)
        if (std::find(stages.begin(), stages.end(), score.stage) == stages.end()) stages.push_back(score.stage);
    std::stable_sort(stages.begin(), stages.end(), [](const std::string& a, const std::string& b) { return stageRank(a) < stageRank(b); });
    for (const auto& stage : stages) {
        // One row per provider, mode and language; one column per metric
        std::vector<std::string> metrics, rows;
        std::map<std::string, std::map<std::string, std::string>> cells;
        std::map<std::string, int> clipCounts;
        for (const auto& score : _scores) {
            if (score.stage != stage) continue;
            if (std::find(metrics.begin(), metrics.end(), score.metric) == metrics.end()) metrics.push_back(score.metric);
            const std::string row = score.provider + " | " + (score.mode.empty() ? "" : score.mode) + " | " + score.language;
            if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
            cells[row][score.metric] = formatValue(score.value, score.unit);
            clipCounts[row] = std::max(clipCounts[row], score.clips);
        }
        out << "## " << stage << "\n\n| provider | mode | language | clips |";
        for (const auto& metric : metrics) out << " " << metric << " |";
        out << "\n| --- | --- | --- | --- |";
        for (size_t i = 0; i < metrics.size(); ++i) out << " --- |";
        out << "\n";
        for (const auto& row : rows) {
            out << "| " << row << " | " << clipCounts[row] << " |";
            for (const auto& metric : metrics) out << " " << (cells[row].count(metric) ? cells[row][metric] : "") << " |";
            out << "\n";
        }
        out << "\n";
        bool any = false;
        for (const auto& note : _notes) {
            if (note.stage != stage) continue;
            if (!any) out << "<details><summary>" << stage << " details</summary>\n\n";
            any = true;
            out << "- " << note.provider << ": " << note.line << "\n";
        }
        if (any) out << "\n</details>\n\n";
    }
    std::ofstream file(path, std::ios::binary);
    file << out.str();
    return file.good();
}

bool Report::writeJson(const std::string& path) const {
    nlohmann::json scores = nlohmann::json::array();
    for (const auto& score : _scores) {
        scores.push_back({{"stage", score.stage}, {"provider", score.provider}, {"mode", score.mode}, {"language", score.language},
                          {"metric", score.metric}, {"value", score.value}, {"higher_is_better", score.higherIsBetter},
                          {"gated", score.gated}, {"unit", unitName(score.unit)}, {"clips", score.clips}});
    }
    nlohmann::json notes = nlohmann::json::array();
    for (const auto& note : _notes) notes.push_back({{"stage", note.stage}, {"provider", note.provider}, {"line", note.line}});
    std::ofstream file(path, std::ios::binary);
    file << nlohmann::json{{"scores", scores}, {"notes", notes}}.dump(2) << "\n";
    return file.good();
}

void Report::print() const {
    for (const auto& score : _scores) {
        std::printf("%-8s %-12s %-10s %-6s %-24s %10s  (%d clips)%s\n", score.stage.c_str(), score.provider.c_str(), score.mode.c_str(),
                    score.language.c_str(), score.metric.c_str(), formatValue(score.value, score.unit).c_str(), score.clips,
                    score.gated ? "" : "  not gated");
    }
}

double defaultMargin(const std::string& stage, const std::string& metric) {
    // Absolute: rates in [0, 1]. Small sets move in big steps (a smoke tier of 10 turns moves by 0.1).
    if (metric == "wer" || metric == "judge_wer") return stage == "stt" ? 0.03 : 0.05;
    if (metric == "cer" || metric == "judge_cer") return 0.03;
    if (metric == "f1" || metric == "precision" || metric == "recall") return 0.05;
    if (metric == "false_alarm") return 0.05;
    if (metric == "accuracy" || metric == "false_end_500ms") return 0.10;
    if (metric == "eer") return 0.05;
    if (metric == "clipped") return 0.001;
    if (metric == "bad_clips") return 0.10;
    if (metric == "noise_attenuation_db") return 3.0;
    return 0.05;
}

CheckResult checkBaselines(const std::vector<Score>& scores, const vasdk::eval::Baselines& baselines, vasdk::eval::Tier tier) {
    CheckResult result;
    for (const auto& score : scores) {
        if (!score.gated) continue;
        const vasdk::eval::Baseline* found = nullptr;
        for (const auto& baseline : baselines.baselines()) {
            if (baseline.tier() == tier && key(baseline.stage(), baseline.provider(), baseline.mode(), baseline.language(), baseline.metric()) == key(score)) {
                found = &baseline;
                break;
            }
        }
        if (!found) {
            result.unchecked.push_back(key(score));
            continue;
        }
        ++result.checked;
        const bool worse = found->higher_is_better() ? score.value < found->value() - found->margin()
                                                     : score.value > found->value() + found->margin();
        if (worse) {
            result.failures.push_back(key(score) + ": " + formatValue(score.value, score.unit) + ", baseline " +
                                      formatValue(found->value(), score.unit) + (found->higher_is_better() ? " - " : " + ") +
                                      formatValue(found->margin(), score.unit));
        }
    }
    return result;
}

void updateBaselines(vasdk::eval::Baselines& baselines, const std::vector<Score>& scores, vasdk::eval::Tier tier) {
    for (const auto& score : scores) {
        if (!score.gated) continue;
        vasdk::eval::Baseline* found = nullptr;
        for (auto& baseline : *baselines.mutable_baselines()) {
            if (baseline.tier() == tier && key(baseline.stage(), baseline.provider(), baseline.mode(), baseline.language(), baseline.metric()) == key(score)) {
                found = &baseline;
                break;
            }
        }
        if (!found) {
            found = baselines.add_baselines();
            found->set_stage(score.stage);
            found->set_provider(score.provider);
            found->set_mode(score.mode);
            found->set_language(score.language);
            found->set_metric(score.metric);
            found->set_tier(tier);
            found->set_higher_is_better(score.higherIsBetter);
            found->set_margin(defaultMargin(score.stage, score.metric));
        }
        found->set_value(std::round(score.value * 10000.0) / 10000.0);   // 4 decimals; a margin edited by hand stays
    }
    std::sort(baselines.mutable_baselines()->begin(), baselines.mutable_baselines()->end(),
              [](const vasdk::eval::Baseline& a, const vasdk::eval::Baseline& b) {
                  if (a.tier() != b.tier()) return a.tier() < b.tier();
                  if (stageRank(a.stage()) != stageRank(b.stage())) return stageRank(a.stage()) < stageRank(b.stage());
                  return key(a.stage(), a.provider(), a.mode(), a.language(), a.metric()) <
                         key(b.stage(), b.provider(), b.mode(), b.language(), b.metric());
              });
}

bool loadBaselines(const std::string& path, vasdk::eval::Baselines& baselines, std::string* error) {
    std::ifstream in(path);
    if (!in) return fail(error, "no " + path);
    std::stringstream text;
    text << in.rdbuf();
    if (!google::protobuf::TextFormat::ParseFromString(text.str(), &baselines)) return fail(error, path + " isn't a vasdk.eval.Baselines");
    return true;
}

bool saveBaselines(const std::string& path, const vasdk::eval::Baselines& baselines, std::string* error) {
    std::ostringstream out;
    out << "# proto-file: VASDK_3P_IMPL/Evaluation/proto/vasdk/eval/eval_set.proto\n"
           "# proto-message: vasdk.eval.Baselines\n"
           "#\n"
           "# What each local provider scored on the evaluation set (docs/evaluation.md), and how much worse\n"
           "# it may get: `vasdk_provider_eval --check` fails past value + margin (value - margin when higher\n"
           "# is better). Written by `vasdk_provider_eval --write-baselines`; margins edited by hand stay.\n";
    google::protobuf::TextFormat::Printer printer;
    printer.SetSingleLineMode(true);
    for (const auto& baseline : baselines.baselines()) {
        std::string line;
        printer.PrintToString(baseline, &line);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out << "baselines { " << line << " }\n";
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) return fail(error, "can't write " + path);
    file << out.str();
    return file.good() || fail(error, "can't write " + path);
}

} // namespace va::thirdparty::evaluation
