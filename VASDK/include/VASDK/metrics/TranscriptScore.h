#pragma once

// How far a transcript is from what was said: word and character error rates.
//
// Both sides are normalized first, so only the words count (normalizeTranscript):
// - lower case in the Latin, Greek and Cyrillic scripts, typographic apostrophes as '
//   (va::nlu::foldedText);
// - punctuation becomes a space ("Hello, world!" → "hello world", "twenty-one" → "twenty one"),
//   except an apostrophe between two letters ("don't", "l'homme");
// - runs of spaces become one.
// Numbers aren't spelled out: "21" and "twenty one" differ, so references and recognizers should
// agree on how numbers are written (the evaluation data spells them).
//
// The error rate is (substitutions + deletions + insertions) / reference length, from a
// Levenshtein alignment. Over a corpus, add up the counts and divide once (EditCounts +=):
// the mean of each clip's rate weighs short clips too much.
//
//   va::metrics::EditCounts total;
//   for (const auto& clip : clips) total += va::metrics::scoreWords(clip.reference, clip.heard);
//   std::printf("WER %.1f%%\n", 100 * total.errorRate());
//
// C: vasdk_score_transcript (VASDK/c/vasdk.h). docs/evaluation.md.

#include <VASDK/vasdk_export.h>

#include <cstddef>
#include <string>
#include <vector>

namespace va {
namespace metrics {

struct EditCounts {
    size_t substitutions = 0;
    size_t deletions = 0;        // in the reference, missing from the hypothesis
    size_t insertions = 0;       // in the hypothesis, not in the reference
    size_t referenceLength = 0;  // words (or characters) of the reference

    size_t errors() const { return substitutions + deletions + insertions; }
    /// errors / reference length; 0 for an empty reference heard as nothing, 1 per inserted unit otherwise
    double errorRate() const {
        if (referenceLength == 0) return static_cast<double>(insertions);
        return static_cast<double>(errors()) / static_cast<double>(referenceLength);
    }
    EditCounts& operator+=(const EditCounts& other) {
        substitutions += other.substitutions;
        deletions += other.deletions;
        insertions += other.insertions;
        referenceLength += other.referenceLength;
        return *this;
    }
};

/// The text as it is scored: folded, punctuation as spaces, single spaces, no leading or trailing ones
VASDK_EXPORT std::string normalizeTranscript(const std::string& text);

/// The normalized text's words
VASDK_EXPORT std::vector<std::string> transcriptWords(const std::string& text);

/// Word edits from `reference` to `hypothesis`, both normalized
VASDK_EXPORT EditCounts scoreWords(const std::string& reference, const std::string& hypothesis);

/// Character edits (UTF-8 code points, spaces between words included), both normalized
VASDK_EXPORT EditCounts scoreCharacters(const std::string& reference, const std::string& hypothesis);

} // namespace metrics
} // namespace va
