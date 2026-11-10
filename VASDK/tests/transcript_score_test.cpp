// Transcript scores: normalization (case, punctuation, apostrophes, accents, spaces), word and
// character edits counted by kind, and a corpus total that adds counts rather than averaging rates.

#include <VASDK/metrics/TranscriptScore.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace va::metrics;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

void normalizes(const std::string& text, const std::string& expected) {
    const auto got = normalizeTranscript(text);
    check(got == expected, "normalize \"" + text + "\" → \"" + got + "\", expected \"" + expected + "\"");
}

void counts(const std::string& reference, const std::string& hypothesis, size_t substitutions, size_t deletions, size_t insertions,
            size_t length) {
    const auto score = scoreWords(reference, hypothesis);
    check(score.substitutions == substitutions && score.deletions == deletions && score.insertions == insertions &&
              score.referenceLength == length,
          "words \"" + reference + "\" / \"" + hypothesis + "\": S" + std::to_string(score.substitutions) + " D" +
              std::to_string(score.deletions) + " I" + std::to_string(score.insertions) + " N" + std::to_string(score.referenceLength));
}

} // namespace

int main() {
    normalizes("Hello, World!", "hello world");
    normalizes("  twenty-one   apples. ", "twenty one apples");
    normalizes("Don't stop", "don't stop");
    normalizes("Don\xE2\x80\x99t stop", "don't stop");                       // ’
    normalizes("'quoted' words", "quoted words");                             // quotes, not apostrophes
    normalizes("L'HOMME \xC3\x89T\xC3\x89 d\xC3\xA9j\xC3\xA0", "l'homme \xC3\xA9t\xC3\xA9 d\xC3\xA9j\xC3\xA0");  // L'HOMME ÉTÉ déjà
    normalizes("\xC2\xAB Bonjour \xC2\xBB, dit-il\xE2\x80\xA6", "bonjour dit il");  // « Bonjour », dit-il…
    normalizes("3.5 km/h", "3 5 km h");
    normalizes("", "");
    normalizes("?!", "");

    counts("the cat sat on the mat", "the cat sat on the mat", 0, 0, 0, 6);
    counts("the cat sat on the mat", "the cat sit on mat", 1, 1, 0, 6);
    counts("the cat sat", "the big cat sat down", 0, 0, 2, 3);
    counts("The cat, sat.", "the CAT sat", 0, 0, 0, 3);
    counts("a b c", "", 0, 3, 0, 3);
    counts("", "a b", 0, 0, 2, 0);

    const auto wer = scoreWords("the cat sat on the mat", "the cat sit on mat");
    check(std::fabs(wer.errorRate() - 2.0 / 6.0) < 1e-12, "WER 2/6");
    check(scoreWords("", "").errorRate() == 0.0, "nothing heard as nothing: 0");
    check(scoreWords("", "a b").errorRate() == 2.0, "empty reference: one per insertion");

    // Characters: code points, the space between words included
    const auto cer = scoreCharacters("caf\xC3\xA9", "cafe");   // café / cafe: one substitution of 4
    check(cer.substitutions == 1 && cer.referenceLength == 4 && cer.errors() == 1, "CER counts code points, not bytes");
    const auto split = scoreCharacters("ab cd", "abcd");
    check(split.deletions == 1 && split.referenceLength == 5, "CER counts the space");

    // Corpus: counts added up, one rate
    EditCounts total;
    total += scoreWords("a", "b");                                       // 1 of 1
    total += scoreWords("a b c d e f g h i", "a b c d e f g h i");       // 0 of 9
    check(total.errors() == 1 && total.referenceLength == 10 && std::fabs(total.errorRate() - 0.1) < 1e-12,
          "corpus WER 1/10, not the mean of 100% and 0%");

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("transcript_score: all passed\n");
    return 0;
}
