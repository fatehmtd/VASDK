// The C API's transcript scores (VASDK/c/vasdk.h, "Transcript scores") over va::metrics::TranscriptScore.
#include "c/vasdk.h"
#include "metrics/TranscriptScore.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

char* copyText(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out) std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

nlohmann::json counts(const va::metrics::EditCounts& score) {
    return {{"substitutions", score.substitutions},
            {"deletions", score.deletions},
            {"insertions", score.insertions},
            {"reference_length", score.referenceLength},
            {"error_rate", score.errorRate()}};
}

} // namespace

char* vasdk_score_transcript(const char* reference, const char* hypothesis) {
    const std::string said = reference ? reference : "";
    const std::string heard = hypothesis ? hypothesis : "";
    const nlohmann::json result = {{"words", counts(va::metrics::scoreWords(said, heard))},
                                   {"characters", counts(va::metrics::scoreCharacters(said, heard))},
                                   {"reference", va::metrics::normalizeTranscript(said)},
                                   {"hypothesis", va::metrics::normalizeTranscript(heard)}};
    return copyText(result.dump());
}

char* vasdk_normalize_transcript(const char* text) {
    return copyText(va::metrics::normalizeTranscript(text ? text : ""));
}
