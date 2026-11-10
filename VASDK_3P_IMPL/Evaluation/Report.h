#pragma once

// What an evaluation measured, written as a Markdown and a JSON report, and held to baselines
// (baselines.txtpb, a vasdk.eval.Baselines): a gated score may be worse than its baseline by its
// margin and no more; better always passes.

#include "vasdk/eval/eval_set.pb.h"

#include <string>
#include <vector>

namespace va::thirdparty::evaluation {

enum class Unit { Rate, Seconds, Ratio, Count };   // Rate shows as a percentage

struct Score {
    std::string stage;          // "stt", "tts", "vad", "turn", "speaker", "denoise"
    std::string provider;
    std::string mode;           // stt: "oneshot" or "streaming"
    std::string language;
    std::string metric;         // "wer", "f1", "rtf", ...
    double value = 0;
    bool higherIsBetter = false;
    bool gated = false;         // held to a baseline; timings aren't (they measure the machine)
    Unit unit = Unit::Rate;
    int clips = 0;              // how many clips it was measured on
};

class Report {
public:
    void add(Score score) { _scores.push_back(std::move(score)); }
    /// A line under the stage and provider's details: a clip that went wrong, what was heard
    void note(const std::string& stage, const std::string& provider, const std::string& line);
    const std::vector<Score>& scores() const { return _scores; }

    bool writeMarkdown(const std::string& path, const std::string& title) const;
    bool writeJson(const std::string& path) const;
    /// The scores as a table on stdout
    void print() const;

private:
    struct Note {
        std::string stage, provider, line;
    };
    std::vector<Score> _scores;
    std::vector<Note> _notes;
};

/// How much worse than its baseline a metric may get before a check fails
double defaultMargin(const std::string& stage, const std::string& metric);

struct CheckResult {
    std::vector<std::string> failures;   // worse than baseline + margin
    std::vector<std::string> unchecked;  // gated scores no baseline names
    int checked = 0;
};

/// Each gated score against the baseline of the same stage, provider, mode, language, metric and tier
CheckResult checkBaselines(const std::vector<Score>& scores, const vasdk::eval::Baselines& baselines, vasdk::eval::Tier tier);

/// `baselines` with the gated scores of this run put in, replacing those of the same key
void updateBaselines(vasdk::eval::Baselines& baselines, const std::vector<Score>& scores, vasdk::eval::Tier tier);

bool loadBaselines(const std::string& path, vasdk::eval::Baselines& baselines, std::string* error);
bool saveBaselines(const std::string& path, const vasdk::eval::Baselines& baselines, std::string* error);

std::string formatValue(double value, Unit unit);

} // namespace va::thirdparty::evaluation
