#include "nlu/NluProvider.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace va {
namespace nlu {

NluResult CascadeNluProvider::understand(const NluRequest& request) const {
    NluResult best;
    for (const auto& stage : _stages) {
        auto result = stage->understand(request);
        if (result.stage.empty()) result.stage = stage->name();
        // Sure, and of enough of the words: a keyword may stand for a short sentence ("let me talk to a person",
        // up to 6 words a matched word), but a keyword in a long question goes on to the next stage.
        auto sureness = [](const NluResult& of) {
            return of.intents.empty() ? 0.0f : of.intents.front().confidence * std::min(1.0f, 6.0f * of.intents.front().explains);
        };
        if (sureness(result) >= _minConfidence) return result;
        if (result.noneMeant) return result;   // "none of them" beats an earlier stage's weak guess
        const float confidence = sureness(result);
        const float bestConfidence = sureness(best);
        if (best.stage.empty() || confidence > bestConfidence) best = std::move(result);
    }
    return best;
}

float wordSimilarity(const std::string& a, const std::string& b) {
    if (a == b) return 1.0f;
    if (a.empty() || b.empty()) return 0.0f;
    // Damerau-Levenshtein (optimal string alignment): insert, delete, substitute, swap neighbours.
    std::vector<std::vector<size_t>> distance(a.size() + 1, std::vector<size_t>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); ++i) distance[i][0] = i;
    for (size_t j = 0; j <= b.size(); ++j) distance[0][j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            distance[i][j] = std::min({distance[i - 1][j] + 1, distance[i][j - 1] + 1, distance[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                distance[i][j] = std::min(distance[i][j], distance[i - 2][j - 2] + 1);
        }
    }
    return 1.0f - static_cast<float>(distance[a.size()][b.size()]) / static_cast<float>(std::max(a.size(), b.size()));
}

float wordMatch(const std::string& heard, const std::string& expected, float minSimilarity) {
    if (heard == expected) return 1.0f;
    if (heard.size() < 4 || expected.size() < 4 || minSimilarity >= 1.0f) return 0.0f;   // short words: "book" isn't "look"
    auto lettersOnly = [](const std::string& word) {   // "11:00" isn't "14:00", nor "2021" "2026"
        return std::none_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    if (!lettersOnly(heard) || !lettersOnly(expected)) return 0.0f;
    const float similarity = wordSimilarity(heard, expected);
    return similarity >= minSimilarity ? similarity : 0.0f;
}

std::pair<std::string, std::string> doubleMetaphone(const std::string& word) {
    // Lawrence Philips' Double Metaphone: a primary code, and an alternate where a spelling has
    // two common sounds ("Schmidt": XMT, SMT). Letters only, up to 4 sounds each.
    std::string text;
    for (const unsigned char c : word)
        if (std::isalpha(c)) text += static_cast<char>(std::toupper(c));
    const int length = static_cast<int>(text.size());
    const int last = length - 1;
    std::string primary, alternate;
    if (text.empty()) return {primary, alternate};
    text += "     ";   // lookahead past the end reads spaces

    auto letter = [&](int at) { return at < 0 || at >= length ? '\0' : text[static_cast<size_t>(at)]; };
    auto vowel = [&](int at) { return std::string("AEIOUY").find(letter(at)) != std::string::npos && letter(at) != '\0'; };
    // Whether one of `options` is spelled at `at`.
    auto spelled = [&](int at, std::initializer_list<const char*> options) {
        if (at < 0) return false;
        for (const char* option : options)
            if (text.compare(static_cast<size_t>(at), std::strlen(option), option) == 0) return true;
        return false;
    };
    auto add = [&](const char* main, const char* other) {
        primary += main;
        alternate += other;
    };
    auto addBoth = [&](const char* sound) { add(sound, sound); };
    const bool slavoGermanic = text.find('W') != std::string::npos || text.find('K') != std::string::npos ||
                               text.find("CZ") != std::string::npos || text.find("WITZ") != std::string::npos;

    int current = 0;
    if (spelled(0, {"GN", "KN", "PN", "WR", "PS"})) current = 1;   // a silent first letter
    if (letter(0) == 'X') {                                          // "Xavier"
        addBoth("S");
        current = 1;
    }
    while (current < length && (primary.size() < 4 || alternate.size() < 4)) {
        const char c = letter(current);
        switch (c) {
        case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
            if (current == 0) addBoth("A");   // vowels count at the start only
            ++current;
            break;
        case 'B':
            addBoth("P");
            current += letter(current + 1) == 'B' ? 2 : 1;
            break;
        case 'C':
            if (current > 1 && !vowel(current - 2) && spelled(current - 1, {"ACH"}) && letter(current + 2) != 'I' &&
                (letter(current + 2) != 'E' || spelled(current - 2, {"BACHER", "MACHER"}))) {
                addBoth("K");
                current += 2;
            } else if (current == 0 && spelled(current, {"CAESAR"})) {
                addBoth("S");
                current += 2;
            } else if (spelled(current, {"CHIA"})) {
                addBoth("K");
                current += 2;
            } else if (spelled(current, {"CH"})) {
                if (current > 0 && spelled(current, {"CHAE"})) {
                    add("K", "X");
                } else if (current == 0 && (spelled(current + 1, {"HARAC", "HARIS"}) || spelled(current + 1, {"HOR", "HYM", "HIA", "HEM"})) &&
                           !spelled(0, {"CHORE"})) {
                    addBoth("K");
                } else if (spelled(0, {"VAN ", "VON ", "SCH"}) || spelled(current - 2, {"ORCHES", "ARCHIT", "ORCHID"}) ||
                           spelled(current + 2, {"T", "S"}) ||
                           ((spelled(current - 1, {"A", "O", "U", "E"}) || current == 0) &&
                            spelled(current + 2, {"L", "R", "N", "M", "B", "H", "F", "V", "W", " "}))) {
                    addBoth("K");
                } else if (current > 0) {
                    if (spelled(0, {"MC"})) addBoth("K");
                    else add("X", "K");
                } else {
                    addBoth("X");
                }
                current += 2;
            } else if (spelled(current, {"CZ"}) && !spelled(current - 2, {"WICZ"})) {
                add("S", "X");
                current += 2;
            } else if (spelled(current + 1, {"CIA"})) {
                addBoth("X");
                current += 3;
            } else if (spelled(current, {"CC"}) && !(current == 1 && letter(0) == 'M')) {
                if (spelled(current + 2, {"I", "E", "H"}) && !spelled(current + 2, {"HU"})) {
                    if ((current == 1 && letter(current - 1) == 'A') || spelled(current - 1, {"UCCEE", "UCCES"})) addBoth("KS");
                    else addBoth("X");
                    current += 3;
                } else {
                    addBoth("K");
                    current += 2;
                }
            } else if (spelled(current, {"CK", "CG", "CQ"})) {
                addBoth("K");
                current += 2;
            } else if (spelled(current, {"CI", "CE", "CY"})) {
                if (spelled(current, {"CIO", "CIE", "CIA"})) add("S", "X");
                else addBoth("S");
                current += 2;
            } else {
                addBoth("K");
                if (spelled(current + 1, {" C", " Q", " G"})) current += 3;
                else if (spelled(current + 1, {"C", "K", "Q"}) && !spelled(current + 1, {"CE", "CI"})) current += 2;
                else current += 1;
            }
            break;
        case 'D':
            if (spelled(current, {"DG"})) {
                if (spelled(current + 2, {"I", "E", "Y"})) {
                    addBoth("J");
                    current += 3;
                } else {
                    addBoth("TK");
                    current += 2;
                }
            } else {
                addBoth("T");
                current += spelled(current, {"DT", "DD"}) ? 2 : 1;
            }
            break;
        case 'F':
            addBoth("F");
            current += letter(current + 1) == 'F' ? 2 : 1;
            break;
        case 'G':
            if (letter(current + 1) == 'H') {
                if (current > 0 && !vowel(current - 1)) {
                    addBoth("K");
                } else if (current == 0) {
                    addBoth(letter(current + 2) == 'I' ? "J" : "K");
                } else if ((current > 1 && spelled(current - 2, {"B", "H", "D"})) || (current > 2 && spelled(current - 3, {"B", "H", "D"})) ||
                           (current > 3 && spelled(current - 4, {"B", "H"}))) {
                    // silent: "hugh", "bough", "broughton"
                } else if (current > 2 && letter(current - 1) == 'U' && spelled(current - 3, {"C", "G", "L", "R", "T"})) {
                    addBoth("F");   // "laugh", "tough"
                } else if (current > 0 && letter(current - 1) != 'I') {
                    addBoth("K");
                }
                current += 2;
            } else if (letter(current + 1) == 'N') {
                if (current == 1 && vowel(0) && !slavoGermanic) add("KN", "N");
                else if (!spelled(current + 2, {"EY"}) && letter(current + 1) != 'Y' && !slavoGermanic) add("N", "KN");
                else addBoth("KN");
                current += 2;
            } else if (spelled(current + 1, {"LI"}) && !slavoGermanic) {
                add("KL", "L");
                current += 2;
            } else if (current == 0 && (letter(current + 1) == 'Y' ||
                                        spelled(current + 1, {"ES", "EP", "EB", "EL", "EY", "IB", "IL", "IN", "IE", "EI", "ER"}))) {
                add("K", "J");
                current += 2;
            } else if ((spelled(current + 1, {"ER"}) || letter(current + 1) == 'Y') && !spelled(0, {"DANGER", "RANGER", "MANGER"}) &&
                       !spelled(current - 1, {"E", "I"}) && !spelled(current - 1, {"RGY", "OGY"})) {
                add("K", "J");
                current += 2;
            } else if (spelled(current + 1, {"E", "I", "Y"}) || spelled(current - 1, {"AGGI", "OGGI"})) {
                if (spelled(0, {"VAN ", "VON ", "SCH"}) || spelled(current + 1, {"ET"})) addBoth("K");
                else if (spelled(current + 1, {"IER "})) addBoth("J");
                else add("J", "K");
                current += 2;
            } else {
                addBoth("K");
                current += letter(current + 1) == 'G' ? 2 : 1;
            }
            break;
        case 'H':
            if ((current == 0 || vowel(current - 1)) && vowel(current + 1)) {
                addBoth("H");
                current += 2;
            } else {
                ++current;   // silent after a consonant, or before one
            }
            break;
        case 'J':
            if (spelled(current, {"JOSE"}) || spelled(0, {"SAN "})) {
                if ((current == 0 && letter(current + 4) == ' ') || spelled(0, {"SAN "})) addBoth("H");
                else add("J", "H");
                ++current;
                break;
            }
            if (current == 0) add("J", "A");
            else if (vowel(current - 1) && !slavoGermanic && (letter(current + 1) == 'A' || letter(current + 1) == 'O')) add("J", "H");
            else if (current == last) add("J", "");
            else if (!spelled(current + 1, {"L", "T", "K", "S", "N", "M", "B", "Z"}) && !spelled(current - 1, {"S", "K", "L"})) addBoth("J");
            current += letter(current + 1) == 'J' ? 2 : 1;
            break;
        case 'K':
            addBoth("K");
            current += letter(current + 1) == 'K' ? 2 : 1;
            break;
        case 'L':
            if (letter(current + 1) == 'L') {
                if ((current == length - 3 && spelled(current - 1, {"ILLO", "ILLA", "ALLE"})) ||
                    ((spelled(last - 1, {"AS", "OS"}) || spelled(last, {"A", "O"})) && spelled(current - 1, {"ALLE"})))
                    add("L", "");   // Spanish: "cabrillo"
                else
                    addBoth("L");
                current += 2;
            } else {
                addBoth("L");
                ++current;
            }
            break;
        case 'M':
            addBoth("M");
            current += (spelled(current - 1, {"UMB"}) && (current + 1 == last || spelled(current + 2, {"ER"}))) || letter(current + 1) == 'M'
                           ? 2 : 1;
            break;
        case 'N':
            addBoth("N");
            current += letter(current + 1) == 'N' ? 2 : 1;
            break;
        case 'P':
            if (letter(current + 1) == 'H') {
                addBoth("F");
                current += 2;
            } else {
                addBoth("P");
                current += spelled(current + 1, {"P", "B"}) ? 2 : 1;
            }
            break;
        case 'Q':
            addBoth("K");
            current += letter(current + 1) == 'Q' ? 2 : 1;
            break;
        case 'R':
            if (current == last && !slavoGermanic && spelled(current - 2, {"IE"}) && !spelled(current - 4, {"ME", "MA"})) add("", "R");
            else addBoth("R");
            current += letter(current + 1) == 'R' ? 2 : 1;
            break;
        case 'S':
            if (spelled(current - 1, {"ISL", "YSL"})) {   // "island", "carlysle"
                ++current;
            } else if (current == 0 && spelled(current, {"SUGAR"})) {
                add("X", "S");
                ++current;
            } else if (spelled(current, {"SH"})) {
                addBoth(spelled(current + 1, {"HEIM", "HOEK", "HOLM", "HOLZ"}) ? "S" : "X");
                current += 2;
            } else if (spelled(current, {"SIO", "SIA"}) || spelled(current, {"SIAN"})) {
                if (!slavoGermanic) add("S", "X");
                else addBoth("S");
                current += 3;
            } else if ((current == 0 && spelled(current + 1, {"M", "N", "L", "W"})) || spelled(current + 1, {"Z"})) {
                add("S", "X");
                current += spelled(current + 1, {"Z"}) ? 2 : 1;
            } else if (spelled(current, {"SC"})) {
                if (letter(current + 2) == 'H') {
                    if (spelled(current + 3, {"OO", "ER", "EN", "UY", "ED", "EM"})) {
                        if (spelled(current + 3, {"ER", "EN"})) add("X", "SK");
                        else addBoth("SK");
                    } else if (current == 0 && !vowel(3) && letter(3) != 'W') {
                        add("X", "S");
                    } else {
                        addBoth("X");
                    }
                } else if (spelled(current + 2, {"I", "E", "Y"})) {
                    addBoth("S");
                } else {
                    addBoth("SK");
                }
                current += 3;
            } else {
                if (current == last && spelled(current - 2, {"AI", "OI"})) add("", "S");   // French: "Artois"
                else addBoth("S");
                current += spelled(current + 1, {"S", "Z"}) ? 2 : 1;
            }
            break;
        case 'T':
            if (spelled(current, {"TION"})) {
                addBoth("X");
                current += 3;
            } else if (spelled(current, {"TIA", "TCH"})) {
                addBoth("X");
                current += 3;
            } else if (spelled(current, {"TH"}) || spelled(current, {"TTH"})) {
                if (spelled(current + 2, {"OM", "AM"}) || spelled(0, {"VAN ", "VON ", "SCH"})) addBoth("T");   // "Thomas"
                else add("0", "T");
                current += 2;
            } else {
                addBoth("T");
                current += spelled(current + 1, {"T", "D"}) ? 2 : 1;
            }
            break;
        case 'V':
            addBoth("F");
            current += letter(current + 1) == 'V' ? 2 : 1;
            break;
        case 'W':
            if (spelled(current, {"WR"})) {
                addBoth("R");
                current += 2;
                break;
            }
            if (current == 0 && (vowel(current + 1) || spelled(current, {"WH"}))) {
                if (vowel(current + 1)) add("A", "F");
                else addBoth("A");
            }
            if ((current == last && vowel(current - 1)) || spelled(current - 1, {"EWSKI", "EWSKY", "OWSKI", "OWSKY"}) || spelled(0, {"SCH"})) {
                add("", "F");
                ++current;
            } else if (spelled(current, {"WICZ", "WITZ"})) {
                add("TS", "FX");
                current += 4;
            } else {
                ++current;
            }
            break;
        case 'X':
            if (!(current == last && (spelled(current - 3, {"IAU", "EAU"}) || spelled(current - 2, {"AU", "OU"})))) addBoth("KS");
            current += spelled(current + 1, {"C", "X"}) ? 2 : 1;
            break;
        case 'Z':
            if (letter(current + 1) == 'H') {
                addBoth("J");
                current += 2;
                break;
            }
            if (spelled(current + 1, {"ZO", "ZI", "ZA"}) || (slavoGermanic && current > 0 && letter(current - 1) != 'T')) add("S", "TS");
            else addBoth("S");
            current += letter(current + 1) == 'Z' ? 2 : 1;
            break;
        default:
            ++current;
            break;
        }
    }
    if (primary.size() > 4) primary.resize(4);
    if (alternate.size() > 4) alternate.resize(4);
    return {primary, alternate};
}

std::string phoneticKey(const std::string& word) { return doubleMetaphone(word).first; }

bool soundsAlike(const std::string& a, const std::string& b) {
    const auto [aMain, aOther] = doubleMetaphone(a);
    const auto [bMain, bOther] = doubleMetaphone(b);
    if (aMain.empty() || bMain.empty()) return false;
    return aMain == bMain || aMain == bOther || aOther == bMain || (!aOther.empty() && aOther == bOther);
}

std::vector<std::pair<size_t, float>> findPhrase(const std::vector<std::string>& words, const std::vector<std::string>& phrase,
                                                 float minSimilarity) {
    std::vector<std::pair<size_t, float>> found;
    if (phrase.empty() || phrase.size() > words.size()) return found;
    for (size_t start = 0; start + phrase.size() <= words.size(); ++start) {
        float total = 0;
        size_t i = 0;
        for (; i < phrase.size(); ++i) {
            const float match = wordMatch(words[start + i], phrase[i], minSimilarity);
            if (match == 0.0f) break;
            total += match;
        }
        if (i == phrase.size()) found.emplace_back(start, total);
    }
    return found;
}

} // namespace nlu
} // namespace va
