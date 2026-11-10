#include <VASDK/metrics/TranscriptScore.h>

#include <VASDK/nlu/Language.h>

#include <algorithm>
#include <cstdint>

namespace va {
namespace metrics {

namespace {

// UTF-8 text as code points; a byte that isn't UTF-8 stands for itself.
std::vector<uint32_t> codePoints(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        bool valid = length > 0 && i + length <= text.size();
        uint32_t c = length == 1 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
        for (size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) valid = false;
            c = (c << 6) | (next & 0x3F);
        }
        out.push_back(valid ? c : lead);
        i += valid ? length : 1;
    }
    return out;
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Punctuation, symbols and spaces: what separates words. Letters, digits and marks of any script
// are kept; so are code points this doesn't know, which keeps scripts it hasn't heard of whole.
bool separates(uint32_t c) {
    if (c < 0x80) return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
    if (c >= 0x80 && c <= 0xBF) return true;               // Latin-1 punctuation and symbols: « » ¿ ¡ · ° ...
    if (c == 0xD7 || c == 0xF7) return true;               // × ÷
    if (c >= 0x2000 && c <= 0x2BFF) return true;           // spaces, dashes, quotes, symbols, arrows
    if (c >= 0x3000 && c <= 0x303F) return true;           // CJK punctuation
    if (c >= 0xFE30 && c <= 0xFE6F) return true;           // CJK compatibility forms, small forms
    if (c >= 0xFF01 && c <= 0xFF0F) return true;           // fullwidth ASCII punctuation
    if (c >= 0xFF1A && c <= 0xFF20) return true;
    return false;
}

bool isLetterOrDigit(uint32_t c) { return c != '\'' && !separates(c); }

template <typename T>
EditCounts align(const std::vector<T>& reference, const std::vector<T>& hypothesis) {
    // Levenshtein with backtracking: one row of costs per reference unit, kept for the walk back.
    const size_t rows = reference.size() + 1, columns = hypothesis.size() + 1;
    std::vector<size_t> cost(rows * columns);
    for (size_t i = 0; i < rows; ++i) cost[i * columns] = i;
    for (size_t j = 0; j < columns; ++j) cost[j] = j;
    for (size_t i = 1; i < rows; ++i) {
        for (size_t j = 1; j < columns; ++j) {
            const size_t substitute = cost[(i - 1) * columns + j - 1] + (reference[i - 1] == hypothesis[j - 1] ? 0 : 1);
            const size_t remove = cost[(i - 1) * columns + j] + 1;
            const size_t insert = cost[i * columns + j - 1] + 1;
            cost[i * columns + j] = std::min({substitute, remove, insert});
        }
    }
    EditCounts counts;
    counts.referenceLength = reference.size();
    size_t i = reference.size(), j = hypothesis.size();
    while (i > 0 || j > 0) {
        const size_t here = cost[i * columns + j];
        if (i > 0 && j > 0 && here == cost[(i - 1) * columns + j - 1] + (reference[i - 1] == hypothesis[j - 1] ? 0 : 1)) {
            if (reference[i - 1] != hypothesis[j - 1]) ++counts.substitutions;
            --i;
            --j;
        } else if (i > 0 && here == cost[(i - 1) * columns + j] + 1) {
            ++counts.deletions;
            --i;
        } else {
            ++counts.insertions;
            --j;
        }
    }
    return counts;
}

} // namespace

std::string normalizeTranscript(const std::string& text) {
    const auto points = codePoints(nlu::foldedText(text));
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (size_t i = 0; i < points.size(); ++i) {
        const uint32_t c = points[i];
        // An apostrophe stays between two letters ("don't", "l'homme"); elsewhere it is a quote.
        const bool keptApostrophe = c == '\'' && i > 0 && i + 1 < points.size() && isLetterOrDigit(points[i - 1]) &&
                                    isLetterOrDigit(points[i + 1]);
        if (!keptApostrophe && separates(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        appendUtf8(out, c);
    }
    return out;
}

std::vector<std::string> transcriptWords(const std::string& text) {
    std::vector<std::string> words;
    const std::string normalized = normalizeTranscript(text);
    for (size_t start = 0; start < normalized.size();) {
        const size_t end = std::min(normalized.find(' ', start), normalized.size());
        words.push_back(normalized.substr(start, end - start));
        start = end + 1;
    }
    return words;
}

EditCounts scoreWords(const std::string& reference, const std::string& hypothesis) {
    return align(transcriptWords(reference), transcriptWords(hypothesis));
}

EditCounts scoreCharacters(const std::string& reference, const std::string& hypothesis) {
    return align(codePoints(normalizeTranscript(reference)), codePoints(normalizeTranscript(hypothesis)));
}

} // namespace metrics
} // namespace va
