#pragma once

// The evaluation set on disk: references.txtpb (a vasdk.eval.EvalSet) and the WAV clips beside it,
// written by tools/eval-data/build_eval_set.py and downloaded as model eval_audio_v1 of
// VASDK_3P_IMPL/Evaluation/data/manifest.json (docs/evaluation.md).

#include "vasdk/eval/eval_set.pb.h"

#include <string>
#include <vector>

namespace va::thirdparty::evaluation {

constexpr int kSampleRate = 16000;           // every clip: 16 kHz mono 16-bit
constexpr const char* kSetFolder = "vasdk_eval_v1";
constexpr const char* kSetModel = "eval_audio_v1";

class EvalData {
public:
    /// Reads <folder>/references.txtpb
    bool load(const std::string& folder, std::string* error);

    /// The clips of a kind in a tier (FULL: all of them), of one language when it isn't empty
    std::vector<const vasdk::eval::Clip*> clips(vasdk::eval::ClipKind kind, vasdk::eval::Tier tier,
                                                const std::string& language = {}) const;
    /// The languages of a kind's clips, in the order they first appear
    std::vector<std::string> languages(vasdk::eval::ClipKind kind, vasdk::eval::Tier tier) const;

    /// A clip's samples at 16 kHz mono
    bool audio(const vasdk::eval::Clip& clip, std::vector<short>& pcm, std::string* error) const;

    const std::string& folder() const { return _folder; }
    const vasdk::eval::EvalSet& set() const { return _set; }

private:
    std::string _folder;
    vasdk::eval::EvalSet _set;
};

/// The set's folder: `data` when given; else, with `store` given, the set pulled into that store
/// through `manifest` (the model store checks every file's SHA-256). Empty, with `error` set, when
/// neither has it.
std::string locateData(const std::string& data, const std::string& store, const std::string& manifest, std::string* error);

} // namespace va::thirdparty::evaluation
