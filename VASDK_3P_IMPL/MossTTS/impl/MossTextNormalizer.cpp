#include "MossTextNormalizer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace moss::text {

using U = std::u32string;

// ===========================================================================
// UTF-8
// ===========================================================================

namespace detail {

std::u32string toU32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        int extra = 0;
        if      (c < 0x80)         { cp = c;        extra = 0; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else                       { ++i; continue; } // invalid lead byte
        if (i + extra >= s.size()) break; // truncated sequence
        for (int k = 1; k <= extra; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += 1 + extra;
    }
    return out;
}

std::string toUtf8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t cp : s) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace detail

using detail::toU32;
using detail::toUtf8;

// ===========================================================================
// Character classes (Python `re` / `unicodedata` semantics, approximated where noted)
// ===========================================================================

namespace {

bool inStr(char32_t c, const char32_t* set) {
    for (; *set; ++set) if (*set == c) return true;
    return false;
}

// str.isspace() / regex \s
bool isWs(char32_t c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F) || c == 0x85 ||
           c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool isAsciiDigit(char32_t c)  { return c >= '0' && c <= '9'; }
bool isAsciiLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiAlnum(char32_t c)  { return isAsciiDigit(c) || isAsciiLetter(c); }
bool isAsciiWord(char32_t c)   { return isAsciiAlnum(c) || c == '_'; } // [A-Za-z0-9_]

// regex \d (Unicode decimal digits; the scripts that matter here)
bool isDigit(char32_t c) {
    return isAsciiDigit(c) || (c >= 0xFF10 && c <= 0xFF19) || (c >= 0x0660 && c <= 0x0669) ||
           (c >= 0x06F0 && c <= 0x06F9) || (c >= 0x0966 && c <= 0x096F);
}

// The robust normalizer's _CJK: ideographs + kana
bool isCjk(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF);
}
bool isHan(char32_t c)  { return c >= 0x3400 && c <= 0x9FFF; }
bool isKana(char32_t c) { return c >= 0x3040 && c <= 0x30FF; }

// regex \w: Unicode letters / digits / underscore (letters approximated by script ranges)
bool isWord(char32_t c) {
    if (c < 0x80) return isAsciiWord(c);
    return (c >= 0x00C0 && c <= 0x024F && c != 0xD7 && c != 0xF7) || c == 0xAA || c == 0xB5 || c == 0xBA ||
           (c >= 0x0370 && c <= 0x03FF) || (c >= 0x0400 && c <= 0x052F) || (c >= 0x0530 && c <= 0x058F) ||
           (c >= 0x0590 && c <= 0x05FF) || (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0900 && c <= 0x0DFF) ||
           (c >= 0x0E00 && c <= 0x0EFF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x1E00 && c <= 0x1FFF) ||
           (c >= 0x3040 && c <= 0x30FF && c != 0x30FB) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A) ||
           (c >= 0xFF66 && c <= 0xFF9F) || (c >= 0x20000 && c <= 0x3FFFF) || c == 0x3005 || c == 0x3007;
}

// unicodedata.category(c).startswith("C")
bool isOtherCategory(char32_t c) {
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || c == 0xAD || (c >= 0x600 && c <= 0x605) ||
           c == 0x61C || c == 0x6DD || c == 0x70F || c == 0x180E || (c >= 0x200B && c <= 0x200F) ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x206F) ||
           c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) || (c >= 0xD800 && c <= 0xF8FF) ||
           c == 0xE0001 || (c >= 0xE0020 && c <= 0xE007F) || c >= 0xF0000 ||
           (c >= 0xFFF0 && c <= 0xFFF8) || c == 0xFFFE || c == 0xFFFF;
}

// unicodedata.category(c).startswith("P")
bool isPunctCategory(char32_t c) {
    if (c < 0x80) return inStr(c, U"!\"#%&'()*,-./:;?@[\\]_{}");
    return c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 || c == 0xBB || c == 0xBF ||
           c == 0x37E || c == 0x387 || (c >= 0x55A && c <= 0x55F) || c == 0x589 || c == 0x58A ||
           c == 0x60C || c == 0x61B || c == 0x61F || c == 0x6D4 || c == 0x964 || c == 0x965 ||
           (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x2043) || (c >= 0x2045 && c <= 0x2051) ||
           (c >= 0x2053 && c <= 0x205E) || c == 0x207D || c == 0x207E || c == 0x208D || c == 0x208E ||
           (c >= 0x2308 && c <= 0x230B) || c == 0x2329 || c == 0x232A || (c >= 0x2768 && c <= 0x2775) ||
           (c >= 0x27C5 && c <= 0x27C6) || (c >= 0x27E6 && c <= 0x27EF) || (c >= 0x2983 && c <= 0x2998) ||
           (c >= 0x2E00 && c <= 0x2E4F) || (c >= 0x3001 && c <= 0x3003) || (c >= 0x3008 && c <= 0x3011) ||
           (c >= 0x3014 && c <= 0x301F) || c == 0x3030 || c == 0x303D || c == 0x30A0 || c == 0x30FB ||
           (c >= 0xFE10 && c <= 0xFE19) || (c >= 0xFE30 && c <= 0xFE52) || (c >= 0xFE54 && c <= 0xFE61) ||
           c == 0xFE63 || c == 0xFE68 || c == 0xFE6A || c == 0xFE6B || (c >= 0xFF01 && c <= 0xFF03) ||
           (c >= 0xFF05 && c <= 0xFF0A) || (c >= 0xFF0C && c <= 0xFF0F) || c == 0xFF1A || c == 0xFF1B ||
           c == 0xFF1F || c == 0xFF20 || (c >= 0xFF3B && c <= 0xFF3D) || c == 0xFF3F || c == 0xFF5B ||
           c == 0xFF5D || (c >= 0xFF5F && c <= 0xFF65);
}

U strip(const U& s) {
    size_t a = 0, b = s.size();
    while (a < b && isWs(s[a]))     ++a;
    while (b > a && isWs(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool startsWith(const U& t, size_t i, const U& p) {
    return t.size() >= i + p.size() && t.compare(i, p.size(), p) == 0;
}

// ===========================================================================
// robustNormalize - port of tts_robust_normalizer_single_script.py
// ===========================================================================

// Length of a "___PROT<n>___" placeholder at i, or 0.
size_t placeholderAt(const U& t, size_t i) {
    const U head = U"___PROT";
    if (!startsWith(t, i, head)) return 0;
    size_t j = i + head.size();
    const size_t d = j;
    while (j < t.size() && isAsciiDigit(t[j])) ++j;
    if (j == d || !startsWith(t, j, U"___")) return 0;
    return j + 3 - i;
}

U ensureTerminalPunctuation(const U& text) {
    if (text.empty()) return text;
    const char32_t* const closers = U"\"')]}\uFF09\u3011\u300B\u3009\u300D\u300F\u201D\u2019"; // "')]}）】》〉」』”’
    long idx = static_cast<long>(text.size()) - 1;
    while (idx >= 0 && isWs(text[idx])) --idx;
    while (idx >= 0 && inStr(text[idx], closers)) --idx;
    if (idx >= 0 && isPunctCategory(text[idx])) return text;
    return text + U"\u3002";
}

U baseCleanup(const U& in) {
    U out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t c = in[i];
        if (c == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
            c = '\n';
        }
        if (c == 0x3000) c = ' ';
        if ((c >= 0x200B && c <= 0x200D) || c == 0xFEFF) continue;
        if (c == '\n' || c == '\t' || c == ' ' || !isOtherCategory(c)) out += c;
    }
    return out;
}

// \s*(?:<[-=]+>|[-=]+>|<[-=]+|[→←↔⇒⇐⇔⟶⟵⟷⟹⟸⟺↦↤↪↩])\s*  ->  ，
size_t arrowAt(const U& t, size_t i) {
    const char32_t* const arrows =
        U"\u2192\u2190\u2194\u21D2\u21D0\u21D4\u27F6\u27F5\u27F7\u27F9\u27F8\u27FA\u21A6\u21A4\u21AA\u21A9";
    const char32_t c = t[i];
    auto runEnd = [&](size_t j) { while (j < t.size() && (t[j] == '-' || t[j] == '=')) ++j; return j; };
    if (c == '<') {
        const size_t e = runEnd(i + 1);
        if (e == i + 1) return 0;
        if (e < t.size() && t[e] == '>') return e + 1 - i;
        return e - i;
    }
    if (c == '-' || c == '=') {
        const size_t e = runEnd(i);
        if (e < t.size() && t[e] == '>') return e + 1 - i;
        return 0;
    }
    return inStr(c, arrows) ? 1 : 0;
}

U normalizeFlowArrows(const U& t) {
    U out;
    for (size_t i = 0; i < t.size();) {
        const size_t n = arrowAt(t, i);
        if (n == 0) { out += t[i++]; continue; }
        while (!out.empty() && isWs(out.back())) out.pop_back();
        i += n;
        while (i < t.size() && isWs(t[i])) ++i;
        out += U'\uFF0C';
    }
    return out;
}

U normalizeMarkdownAndLines(const U& in) {
    // [text](https://url) -> text url
    U t;
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '[') {
            size_t j = i + 1;
            while (j < in.size() && in[j] != '[' && in[j] != ']') ++j;
            if (j < in.size() && in[j] == ']' && j > i + 1 && j + 1 < in.size() && in[j + 1] == '(') {
                size_t k = j + 2;
                size_t scheme = startsWith(in, k, U"https://") ? 8 : startsWith(in, k, U"http://") ? 7 : 0;
                if (scheme) {
                    size_t e = k + scheme;
                    while (e < in.size() && in[e] != ')' && !isWs(in[e])) ++e;
                    if (e > k + scheme && e < in.size() && in[e] == ')') {
                        t += in.substr(i + 1, j - i - 1) + U" " + in.substr(k, e - k);
                        i = e + 1;
                        continue;
                    }
                }
            }
        }
        t += in[i++];
    }

    std::vector<U> lines;
    U cur;
    auto flush = [&]() {
        U line = strip(cur);
        cur.clear();
        if (line.empty()) return;
        size_t p = 0;
        // ^#{1,6}\s+
        size_t h = 0;
        while (h < line.size() && line[h] == '#') ++h;
        if (h >= 1 && h <= 6 && h < line.size() && isWs(line[h])) {
            p = h;
            while (p < line.size() && isWs(line[p])) ++p;
            line = line.substr(p);
        }
        // ^>\s+
        if (line.size() > 1 && line[0] == '>' && isWs(line[1])) {
            p = 1;
            while (p < line.size() && isWs(line[p])) ++p;
            line = line.substr(p);
        }
        // ^[-*+]\s+
        if (line.size() > 1 && inStr(line[0], U"-*+") && isWs(line[1])) {
            p = 1;
            while (p < line.size() && isWs(line[p])) ++p;
            line = line.substr(p);
        }
        // ^\d+[.)]\s+
        size_t d = 0;
        while (d < line.size() && isDigit(line[d])) ++d;
        if (d > 0 && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && isWs(line[d + 1])) {
            p = d + 1;
            while (p < line.size() && isWs(line[p])) ++p;
            line = line.substr(p);
        }
        lines.push_back(std::move(line));
    };
    for (char32_t c : t) {
        if (c == '\n' || c == 0x2028 || c == 0x2029) flush();
        else cur += c;
    }
    flush();
    if (lines.empty()) return U();

    U merged = lines[0];
    for (size_t k = 1; k < lines.size(); ++k) merged = ensureTerminalPunctuation(merged) + lines[k];
    return merged;
}

// --- Protected spans ---------------------------------------------------------

size_t matchUrl(const U& t, size_t i) {
    const size_t scheme = startsWith(t, i, U"https://") ? 8 : startsWith(t, i, U"http://") ? 7 : 0;
    if (!scheme) return 0;
    size_t e = i + scheme;
    while (e < t.size() && !isWs(t[e]) &&
           !inStr(t[e], U"\uFF0C\u3002\uFF01\uFF1F\uFF1B\u3001\uFF09\u3011\u300B\u3009\u300D\u300F")) ++e;
    return e > i + scheme ? e - i : 0;
}

bool emailLocal(char32_t c)  { return isAsciiAlnum(c) || inStr(c, U"._%+-"); }
bool emailDomain(char32_t c) { return isAsciiAlnum(c) || c == '.' || c == '-'; }

// (?<![\w.+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}(?![\w.-])
size_t matchEmail(const U& t, size_t i) {
    if (i > 0 && (isWord(t[i - 1]) || inStr(t[i - 1], U".+-"))) return 0;
    size_t e = i;
    while (e < t.size() && emailLocal(t[e])) ++e;
    if (e == i || e >= t.size() || t[e] != '@') return 0;
    const size_t ds = e + 1;
    size_t f = ds;
    while (f < t.size() && emailDomain(t[f])) ++f;
    if (f == ds) return 0;
    if (f < t.size() && isWord(t[f])) return 0;
    size_t dot = f;
    while (dot > ds && t[dot - 1] != '.') --dot;
    if (dot <= ds + 1) return 0;       // need a non-empty part before the last '.'
    if (f - dot < 2) return 0;
    for (size_t k = dot; k < f; ++k) if (!isAsciiLetter(t[k])) return 0;
    return f - i;
}

// (?<![A-Za-z0-9_])@[A-Za-z0-9_]{1,32}
size_t matchMention(const U& t, size_t i) {
    if (t[i] != '@' || (i > 0 && isAsciiWord(t[i - 1]))) return 0;
    size_t e = i + 1;
    while (e < t.size() && e - i - 1 < 32 && isAsciiWord(t[e])) ++e;
    return e > i + 1 ? e - i : 0;
}

// (?<![A-Za-z0-9_])(?:u|r)/[A-Za-z0-9_]+
size_t matchReddit(const U& t, size_t i) {
    if ((t[i] != 'u' && t[i] != 'r') || (i > 0 && isAsciiWord(t[i - 1]))) return 0;
    if (i + 1 >= t.size() || t[i + 1] != '/') return 0;
    size_t e = i + 2;
    while (e < t.size() && isAsciiWord(t[e])) ++e;
    return e > i + 2 ? e - i : 0;
}

// (?<![A-Za-z0-9_])#(?!\s)[^\s#]+
size_t matchHashtag(const U& t, size_t i) {
    if (t[i] != '#' || (i > 0 && isAsciiWord(t[i - 1]))) return 0;
    size_t e = i + 1;
    while (e < t.size() && !isWs(t[e]) && t[e] != '#') ++e;
    return e > i + 1 ? e - i : 0;
}

bool dotTokenChar(char32_t c) { return isAsciiAlnum(c) || inStr(c, U"._-"); }

// (?<![A-Za-z0-9_])\.(?=[A-Za-z0-9._-]*[A-Za-z0-9])[A-Za-z0-9._-]+
size_t matchDotToken(const U& t, size_t i) {
    if (t[i] != '.' || (i > 0 && isAsciiWord(t[i - 1]))) return 0;
    size_t e = i + 1;
    bool alnum = false;
    while (e < t.size() && dotTokenChar(t[e])) { alnum |= isAsciiAlnum(t[e]); ++e; }
    return (e > i + 1 && alnum) ? e - i : 0;
}

bool fileChar(char32_t c) { return isAsciiAlnum(c) || inStr(c, U"._/+:-"); }

// (?<![A-Za-z0-9_])(?=C*[A-Za-z])(?=C*[./+:-])[A-Za-z0-9]C*(?![A-Za-z0-9_]),  C = [A-Za-z0-9._/+:-]
size_t matchFilelike(const U& t, size_t i) {
    if (!isAsciiAlnum(t[i]) || (i > 0 && isAsciiWord(t[i - 1]))) return 0;
    size_t e = i;
    bool letter = false, sep = false;
    while (e < t.size() && fileChar(t[e])) {
        letter |= isAsciiLetter(t[e]);
        sep    |= inStr(t[e], U"./+:-");
        ++e;
    }
    if (!letter || !sep) return 0;
    if (e >= t.size() || !isAsciiWord(t[e])) return e - i;
    // Followed by '_': back off to the last separator inside the run.
    for (size_t k = e - 1; k > i; --k)
        if (inStr(t[k], U"./+:-")) return k - i;
    return 0;
}

using Matcher = size_t (*)(const U&, size_t);

U protectSpans(const U& in, std::vector<U>& protectedSpans) {
    const Matcher matchers[] = {
        matchUrl, matchEmail, matchMention, matchReddit, matchHashtag, matchDotToken, matchFilelike,
    };
    U t = in;
    for (Matcher m : matchers) {
        U out;
        for (size_t i = 0; i < t.size();) {
            const size_t n = m(t, i);
            if (n == 0) { out += t[i++]; continue; }
            out += U"___PROT" + toU32(std::to_string(protectedSpans.size())) + U"___";
            protectedSpans.push_back(t.substr(i, n));
            i += n;
        }
        t = std::move(out);
    }
    return t;
}

U restoreSpans(U t, const std::vector<U>& protectedSpans) {
    for (size_t idx = 0; idx < protectedSpans.size(); ++idx) {
        const U key = U"___PROT" + toU32(std::to_string(idx)) + U"___";
        U out;
        size_t pos = 0;
        for (size_t f; (f = t.find(key, pos)) != U::npos; pos = f + key.size())
            out += t.substr(pos, f - pos) + protectedSpans[idx];
        out += t.substr(pos);
        t = std::move(out);
    }
    return t;
}

U normalizeVisibleUnderscores(const U& t) {
    U out;
    for (size_t i = 0; i < t.size();) {
        if (const size_t n = placeholderAt(t, i)) { out += t.substr(i, n); i += n; continue; }
        out += t[i] == '_' ? U' ' : t[i];
        ++i;
    }
    return out;
}

// --- Spaces ------------------------------------------------------------------

// Drop whitespace runs whose neighbours satisfy before(prev) && after(next).
template <class B, class A>
U dropWsBetween(const U& t, B before, A after) {
    U out;
    for (size_t i = 0; i < t.size();) {
        if (isWs(t[i])) {
            size_t e = i;
            while (e < t.size() && isWs(t[e])) ++e;
            if (i > 0 && e < t.size() && before(t[i - 1]) && after(t[e])) { i = e; continue; }
            out += t.substr(i, e - i);
            i = e;
            continue;
        }
        out += t[i++];
    }
    return out;
}

U collapseSpaces(const U& t) {
    U out;
    for (char32_t c : t) {
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += c;
    }
    return out;
}

// (?:___PROT\d+___|(?=C*[A-Za-z])[A-Za-z0-9]C*) at j - only whether it starts here.
bool latinishStartsAt(const U& t, size_t j) {
    if (j >= t.size()) return false;
    if (placeholderAt(t, j)) return true;
    if (!isAsciiAlnum(t[j])) return false;
    for (size_t k = j; k < t.size() && fileChar(t[k]); ++k)
        if (isAsciiLetter(t[k])) return true;
    return false;
}

U normalizeSpaces(const U& in) {
    // [ \t\r\f\v]+ -> " "
    U t;
    for (size_t i = 0; i < in.size();) {
        if (inStr(in[i], U" \t\r\f\v")) {
            while (i < in.size() && inStr(in[i], U" \t\r\f\v")) ++i;
            t += ' ';
            continue;
        }
        t += in[i++];
    }
    t = dropWsBetween(t, isCjk, isCjk);
    t = dropWsBetween(t, isCjk, isDigit);
    t = dropWsBetween(t, isDigit, isCjk);

    // CJK followed by a latin-ish token: add a space.
    {
        U out;
        for (size_t i = 0; i < t.size(); ++i) {
            out += t[i];
            if (isCjk(t[i]) && latinishStartsAt(t, i + 1)) out += ' ';
        }
        t = std::move(out);
    }
    // Latin-ish token followed by CJK: add a space.
    {
        U out;
        for (size_t i = 0; i < t.size();) {
            if (const size_t n = placeholderAt(t, i)) {
                if (i + n < t.size() && isCjk(t[i + n])) {
                    out += t.substr(i, n) + U" ";
                    i += n;
                    continue;
                }
            } else if (isAsciiAlnum(t[i])) {
                size_t e = i;
                bool letter = false;
                while (e < t.size() && fileChar(t[e])) { letter |= isAsciiLetter(t[e]); ++e; }
                if (letter && e < t.size() && isCjk(t[e])) {
                    out += t.substr(i, e - i) + U" ";
                    i = e;
                    continue;
                }
            }
            out += t[i++];
        }
        t = std::move(out);
    }
    t = collapseSpaces(t);

    auto isClosePunct = [](char32_t c) {
        return inStr(c, U"\uFF0C\u3002\uFF01\uFF1F\uFF1B\uFF1A\u3001\u201D\u2019\u300D\u300F\u3011\uFF09\u300B");
    };
    auto isOpenPunct = [](char32_t c) {
        return inStr(c, U"\uFF08\u3010\u300C\u300E\u300A\u201C\u2018");
    };
    auto isCnPunct = [](char32_t c) {
        return inStr(c, U"\uFF0C\u3002\uFF01\uFF1F\uFF1B\uFF1A\u3001");
    };
    auto any = [](char32_t) { return true; };
    t = dropWsBetween(t, any, isClosePunct);                                  // \s+([，。...])
    t = dropWsBetween(t, isOpenPunct, any);                                   // ([（【...])\s+
    {   // ([，。！？；：、])\s*  (also at end of text)
        U out;
        for (size_t i = 0; i < t.size(); ++i) {
            out += t[i];
            if (isCnPunct(t[i])) while (i + 1 < t.size() && isWs(t[i + 1])) ++i;
        }
        t = std::move(out);
    }
    t = dropWsBetween(t, any, [](char32_t c) { return inStr(c, U",.;!?"); }); // \s+([,.;!?])
    return strip(collapseSpaces(t));
}

// --- Structural punctuation ---------------------------------------------------

// open\s*([^excluded]+?)\s*close -> "content"
template <class IsOpen, class IsClose, class IsExcluded>
U quoteBrackets(const U& t, IsOpen isOpen, IsClose isClose, IsExcluded excluded) {
    U out;
    for (size_t i = 0; i < t.size();) {
        if (isOpen(t[i])) {
            size_t j = i + 1;
            while (j < t.size() && !excluded(t[j])) ++j;
            if (j < t.size() && isClose(t[j]) && j > i + 1) {
                const U inner = t.substr(i + 1, j - i - 1);
                size_t a = 0;
                while (a < inner.size() && isWs(inner[a])) ++a;
                if (a == inner.size()) a = inner.size() - 1;
                size_t b = inner.size();
                while (b > a + 1 && isWs(inner[b - 1])) --b;
                out += U"\"" + inner.substr(a, b - a) + U"\"";
                i = j + 1;
                continue;
            }
        }
        out += t[i++];
    }
    return out;
}

U unwrapStandaloneTitles(const U& t) {
    // (^|[。！？!?；;]\s*)《([^》]+)》(?=\s*(?:___PROT\d+___|[—–―-]{2,}|$|[。！？!?；;，,]))
    const char32_t* const sentenceEnd = U"\u3002\uFF01\uFF1F!?\uFF1B;";
    auto isDash = [](char32_t c) { return c == 0x2014 || c == 0x2013 || c == 0x2015 || c == '-'; };
    U out;
    for (size_t i = 0; i < t.size();) {
        if (t[i] == U'\u300A') {
            bool okBefore = (i == 0);
            if (!okBefore) {
                size_t k = i;
                while (k > 0 && isWs(t[k - 1])) --k;
                okBefore = k > 0 && inStr(t[k - 1], sentenceEnd);
            }
            size_t j = i + 1;
            while (j < t.size() && t[j] != U'\u300B') ++j;
            if (okBefore && j < t.size() && j > i + 1) {
                size_t k = j + 1;
                while (k < t.size() && isWs(t[k])) ++k;
                bool okAfter = k >= t.size() || (k + 1 == t.size() && t[k] == '\n') ||
                               placeholderAt(t, k) != 0 ||
                               inStr(t[k], U"\u3002\uFF01\uFF1F!?\uFF1B;\uFF0C,") ||
                               (isDash(t[k]) && k + 1 < t.size() && isDash(t[k + 1]));
                if (okAfter) {
                    out += t.substr(i + 1, j - i - 1);
                    i = j + 1;
                    continue;
                }
            }
        }
        out += t[i++];
    }
    return out;
}

U normalizeStructuralPunctuation(const U& in) {
    U t = quoteBrackets(in, [](char32_t c) { return c == '['; }, [](char32_t c) { return c == ']'; },
                        [](char32_t c) { return c == '[' || c == ']'; });
    t = quoteBrackets(t, [](char32_t c) { return c == '{'; }, [](char32_t c) { return c == '}'; },
                      [](char32_t c) { return c == '{' || c == '}'; });
    auto cjkClose = [](char32_t c) { return inStr(c, U"\u3011\u3017\u300F\u300D"); };
    t = quoteBrackets(t, [](char32_t c) { return inStr(c, U"\u3010\u3016\u300E\u300C"); }, cjkClose, cjkClose);
    t = unwrapStandaloneTitles(t);
    t = normalizeFlowArrows(t);

    // \s*(?:—|–|―|-){2,}\s*  ->  。
    auto isDash = [](char32_t c) { return c == 0x2014 || c == 0x2013 || c == 0x2015 || c == '-'; };
    U out;
    for (size_t i = 0; i < t.size();) {
        if (isDash(t[i]) && i + 1 < t.size() && isDash(t[i + 1])) {
            while (!out.empty() && isWs(out.back())) out.pop_back();
            while (i < t.size() && isDash(t[i])) ++i;
            while (i < t.size() && isWs(t[i])) ++i;
            out += U'\u3002';
            continue;
        }
        out += t[i++];
    }
    return out;
}

// Replace runs (length >= 2) of chars in `set` with `rep`.
U collapseRuns(const U& t, const char32_t* set, const U& rep) {
    U out;
    for (size_t i = 0; i < t.size();) {
        if (inStr(t[i], set) && i + 1 < t.size() && inStr(t[i + 1], set)) {
            while (i < t.size() && inStr(t[i], set)) ++i;
            out += rep;
            continue;
        }
        out += t[i++];
    }
    return out;
}

U normalizeRepeatedPunctuation(const U& in) {
    // (?:\.{3,}|…{2,}|……+) -> 。
    U t;
    for (size_t i = 0; i < in.size();) {
        size_t e = i;
        if (in[i] == '.')    { while (e < in.size() && in[e] == '.') ++e; if (e - i >= 3) { t += U'\u3002'; i = e; continue; } }
        if (in[i] == 0x2026) { while (e < in.size() && in[e] == 0x2026) ++e; if (e - i >= 2) { t += U'\u3002'; i = e; continue; } }
        t += in[i++];
    }
    t = collapseRuns(t, U"\u3002\uFF0E", U"\u3002");
    t = collapseRuns(t, U"\uFF0C,", U"\uFF0C");
    t = collapseRuns(t, U"!\uFF01", U"\uFF01");
    t = collapseRuns(t, U"?\uFF1F", U"\uFF1F");
    // [!?！？]{2,}: ？！ when mixed
    U out;
    const char32_t* const qe = U"!?\uFF01\uFF1F";
    for (size_t i = 0; i < t.size();) {
        if (inStr(t[i], qe) && i + 1 < t.size() && inStr(t[i + 1], qe)) {
            bool q = false, e = false;
            while (i < t.size() && inStr(t[i], qe)) {
                q |= (t[i] == '?' || t[i] == 0xFF1F);
                e |= (t[i] == '!' || t[i] == 0xFF01);
                ++i;
            }
            out += (q && e) ? U"\uFF1F\uFF01" : q ? U"\uFF1F" : U"\uFF01";
            continue;
        }
        out += t[i++];
    }
    return out;
}

U ensureTerminalPunctuationByLine(const U& text) {
    if (text.empty()) return text;
    U out;
    size_t start = 0;
    while (true) {
        const size_t nl = text.find(U'\n', start);
        const U line = strip(text.substr(start, nl == U::npos ? U::npos : nl - start));
        if (!line.empty()) out += ensureTerminalPunctuation(line);
        if (nl == U::npos) break;
        out += U'\n';
        start = nl + 1;
    }
    return strip(out);
}

} // namespace

std::string robustNormalize(const std::string& text) {
    U t = baseCleanup(toU32(text));
    t = normalizeMarkdownAndLines(t);
    t = normalizeFlowArrows(t);
    std::vector<U> protectedSpans;
    t = protectSpans(t, protectedSpans);
    t = normalizeVisibleUnderscores(t);

    t = normalizeSpaces(t);
    t = normalizeStructuralPunctuation(t);
    t = normalizeRepeatedPunctuation(t);
    t = normalizeSpaces(t);

    t = restoreSpans(t, protectedSpans);
    t = strip(t);
    return toUtf8(ensureTerminalPunctuationByLine(t));
}

// ===========================================================================
// rewriteHyphensForZh - port of _rewrite_hyphens_before_zh_wetext
// ===========================================================================

namespace {

// (X)\s*-\s*(?=Y) -> \1 + rep
template <class X, class Y>
U hyphenPass(const U& t, X isX, Y isY, const U& rep) {
    U out;
    for (size_t i = 0; i < t.size();) {
        if (isX(t[i])) {
            size_t j = i + 1;
            while (j < t.size() && isWs(t[j])) ++j;
            if (j < t.size() && t[j] == '-') {
                size_t k = j + 1;
                while (k < t.size() && isWs(t[k])) ++k;
                if (k < t.size() && isY(t[k])) {
                    out += t[i];
                    out += rep;
                    i = k;
                    continue;
                }
            }
        }
        out += t[i++];
    }
    return out;
}

} // namespace

std::string rewriteHyphensForZh(const std::string& text) {
    U t = toU32(text);
    if (t.find(U'-') == U::npos) return text;
    constexpr char32_t keepHyphen = 0xE100; // private-use sentinel for "keep this '-'"
    const U keep(1, keepHyphen);

    // (^\s*)-\s*(?=\d)
    {
        size_t s = 0;
        while (s < t.size() && isWs(t[s])) ++s;
        if (s < t.size() && t[s] == '-') {
            size_t k = s + 1;
            while (k < t.size() && isWs(t[k])) ++k;
            if (k < t.size() && isDigit(t[k])) t = t.substr(0, s) + keep + t.substr(k);
        }
    }
    auto delim = [](char32_t c) {
        return inStr(c, U"=:+*/,(\uFF0C:\uFF1A\uFF1B;\uFF08\u3010[{");
    };
    auto notWsOrHyphen = [](char32_t c) { return !isWs(c) && c != '-'; };
    t = hyphenPass(t, delim, isDigit, keep);
    t = hyphenPass(t, isHan, isDigit, keep);
    t = hyphenPass(t, isDigit, isDigit, keep);
    t = hyphenPass(t, isHan, isHan, U"\uFF0C");
    t = hyphenPass(t, notWsOrHyphen, notWsOrHyphen, U" ");
    t = strip(collapseSpaces(t));
    for (auto& c : t) if (c == keepHyphen) c = '-';
    return toUtf8(t);
}

// ===========================================================================
// Language detection
// ===========================================================================

Lang detectLanguage(const std::string& text, bool englishVoice) {
    const U t = toU32(text);
    if (std::any_of(t.begin(), t.end(), [](char32_t c) { return isKana(c) && c != 0x30FB && c != 0x30FC; }))
        return Lang::Ja;
    if (std::any_of(t.begin(), t.end(), isHan))         return Lang::Zh;
    if (std::any_of(t.begin(), t.end(), isAsciiLetter)) return Lang::En;
    return englishVoice ? Lang::En : Lang::Zh;
}

// ===========================================================================
// expand - semantic spelling-out (WeTextProcessing stand-in)
// ===========================================================================

namespace {

// --- English words -----------------------------------------------------------

// 0..19
const char* enOnes(int n) {
    const char* const words[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                 "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                 "seventeen", "eighteen", "nineteen"};
    return words[n];
}

// tens digit 2..9
const char* enTens(int n) {
    const char* const words[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    return words[n];
}

// 1..12
const char* enMonth(int m) {
    const char* const names[] = {"January", "February", "March", "April", "May", "June", "July",
                                 "August", "September", "October", "November", "December"};
    return names[m - 1];
}

std::string enBelow100(int n) {
    if (n < 20) return enOnes(n);
    std::string s = enTens(n / 10);
    if (n % 10) s += std::string(" ") + enOnes(n % 10);
    return s;
}

std::string enBelow1000(int n) {
    if (n < 100) return enBelow100(n);
    std::string s = std::string(enOnes(n / 100)) + " hundred";
    if (n % 100) s += " " + enBelow100(n % 100);
    return s;
}

std::string enCardinal(uint64_t n) {
    if (n == 0) return "zero";
    const char* const scales[] = {"", " thousand", " million", " billion", " trillion", " quadrillion"};
    std::vector<int> groups;
    for (uint64_t v = n; v; v /= 1000) groups.push_back(static_cast<int>(v % 1000));
    std::string s;
    for (size_t g = groups.size(); g-- > 0;) {
        if (!groups[g]) continue;
        if (!s.empty()) s += ' ';
        s += enBelow1000(groups[g]) + scales[g];
    }
    return s;
}

std::string enDigits(const std::string& digits) {
    std::string s;
    for (char c : digits) {
        if (c < '0' || c > '9') continue;
        if (!s.empty()) s += ' ';
        s += enOnes(c - '0');
    }
    return s;
}

std::string enInt(const std::string& digits) {
    if (digits.size() > 15) return enDigits(digits);
    return enCardinal(std::stoull(digits));
}

std::string enOrdinalize(std::string words) {
    const std::pair<const char*, const char*> irregular[] = {
        {"one", "first"}, {"two", "second"}, {"three", "third"}, {"five", "fifth"},
        {"eight", "eighth"}, {"nine", "ninth"}, {"twelve", "twelfth"},
    };
    const size_t sp = words.rfind(' ');
    const std::string head = sp == std::string::npos ? "" : words.substr(0, sp + 1);
    std::string last = sp == std::string::npos ? words : words.substr(sp + 1);
    for (const auto& [card, ord] : irregular)
        if (last == card) return head + ord;
    if (!last.empty() && last.back() == 'y') return head + last.substr(0, last.size() - 1) + "ieth";
    return head + last + "th";
}

std::string enYear(int y) {
    if (y >= 2000 && y <= 2009) return y == 2000 ? "two thousand" : "two thousand " + std::string(enOnes(y % 10));
    const int hi = y / 100, lo = y % 100;
    if (lo == 0) return enBelow100(hi) + " hundred";
    if (lo < 10) return enBelow100(hi) + " oh " + enOnes(lo);
    return enBelow100(hi) + " " + enBelow100(lo);
}

std::string enDecimal(const std::string& ip, const std::string& fp) {
    return enInt(ip) + " point " + enDigits(fp);
}

// --- Chinese / Japanese words ------------------------------------------------

char32_t zhDigit(int d) { return U"零一二三四五六七八九"[d]; }

U zhSection(int n, bool ja) {
    const char32_t* const units[] = {U"", U"十", U"百", U"千"};
    U s;
    bool started = false, zero = false;
    for (int p = 3; p >= 0; --p) {
        int d = n;
        for (int k = 0; k < p; ++k) d /= 10;
        d %= 10;
        if (d == 0) { if (started) zero = true; continue; }
        if (zero && !ja) s += U'零';
        zero = false;
        if (!(ja && d == 1 && p > 0)) s += zhDigit(d);
        s += units[p];
        started = true;
    }
    return s;
}

U zhCardinal(uint64_t n, bool ja) {
    if (n == 0) return U"零";
    const char32_t* const zhBig[] = {U"", U"万", U"亿", U"万亿"};
    const char32_t* const jaBig[] = {U"", U"万", U"億", U"兆"};
    std::vector<int> groups;
    for (uint64_t v = n; v; v /= 10000) groups.push_back(static_cast<int>(v % 10000));
    U s;
    bool needZero = false;
    for (size_t g = groups.size(); g-- > 0;) {
        const int gv = groups[g];
        if (gv == 0) { if (!s.empty()) needZero = true; continue; }
        if (!ja && !s.empty() && (needZero || gv < 1000)) s += U'零';
        // Japanese keeps 一 before the big units (一万, 一億).
        U sec = zhSection(gv, ja);
        if (ja && g > 0 && gv == 1) sec = U"一";
        s += sec + (ja ? jaBig[g] : zhBig[g]);
        needZero = false;
    }
    if (!ja && s.size() >= 2 && s[0] == U'一' && s[1] == U'十') s.erase(0, 1); // 十五, not 一十五
    return s;
}

U zhDigits(const std::string& digits, bool ja) {
    U s;
    for (char c : digits)
        if (c >= '0' && c <= '9') s += (ja && c == '0') ? U'〇' : zhDigit(c - '0');
    return s;
}

U zhInt(const std::string& digits, bool ja) {
    if (digits.size() > 16) return zhDigits(digits, ja);
    return zhCardinal(std::stoull(digits), ja);
}

U zhDecimal(const std::string& ip, const std::string& fp, bool ja) {
    return zhInt(ip, ja) + U"点" + zhDigits(fp, ja);
}

// --- Scanning helpers ----------------------------------------------------------

struct Num {
    size_t      end = 0;
    std::string ip;          // integer digits, commas removed
    std::string fp;          // fraction digits
    bool        comma = false;
};

size_t digitEnd(const U& t, size_t j) {
    while (j < t.size() && isAsciiDigit(t[j])) ++j;
    return j;
}

std::string ascii(const U& t, size_t a, size_t b) {
    std::string s;
    for (size_t k = a; k < b; ++k) if (t[k] < 0x80) s += static_cast<char>(t[k]);
    return s;
}

// \d+(,\d{3})*(\.\d+)?
Num parseNum(const U& t, size_t j) {
    Num n;
    size_t k = digitEnd(t, j);
    n.ip = ascii(t, j, k);
    if (k - j <= 3) {
        while (k < t.size() && t[k] == ',' && digitEnd(t, k + 1) == k + 4) {
            n.ip += ascii(t, k + 1, k + 4);
            n.comma = true;
            k += 4;
        }
    }
    if (k + 1 < t.size() && t[k] == '.' && isAsciiDigit(t[k + 1])) {
        const size_t e = digitEnd(t, k + 1);
        n.fp = ascii(t, k + 1, e);
        k = e;
    }
    n.end = k;
    return n;
}

std::string lowerAscii(const U& t, size_t a, size_t b) {
    std::string s;
    for (size_t k = a; k < b && k < t.size(); ++k) {
        char32_t c = t[k];
        if (c >= 'A' && c <= 'Z') c += 32;
        s += c < 0x80 ? static_cast<char>(c) : '?';
    }
    return s;
}

// ASCII word ending right before i (skipping spaces), lowercased.
std::string prevWord(const U& t, size_t i) {
    size_t e = i;
    while (e > 0 && t[e - 1] == ' ') --e;
    size_t s = e;
    while (s > 0 && isAsciiLetter(t[s - 1])) --s;
    return lowerAscii(t, s, e);
}

bool letterAt(const U& t, size_t k) { return k < t.size() && isAsciiLetter(t[k]); }

// Case-insensitive ASCII match of `w` at k, not followed by a letter.
bool wordAt(const U& t, size_t k, const char* w) {
    const size_t n = std::strlen(w);
    if (k + n > t.size()) return false;
    for (size_t q = 0; q < n; ++q) {
        char32_t c = t[k + q];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != static_cast<unsigned char>(w[q])) return false;
    }
    return !letterAt(t, k + n);
}

// am / pm / a.m. / p.m. at k (after optional single space); returns consumed length incl. space.
size_t meridiemAt(const U& t, size_t k, bool& pm) {
    size_t s = (k < t.size() && t[k] == ' ') ? k + 1 : k;
    for (const char* w : {"a.m.", "p.m.", "am", "pm"}) {
        if (wordAt(t, s, w)) { pm = w[0] == 'p'; return s + std::strlen(w) - k; }
    }
    return 0;
}

struct Unit { const char* sym; const char* en1; const char* enN; const char* zh; const char* ja; bool attachedOnly; };


// Unit right after a number at k (attached, or after one space when allowed).
std::optional<Unit> unitAt(const U& t, size_t k, size_t& used) {
    const Unit units[] = {
        {"km/h", "kilometer per hour", "kilometers per hour", "千米每小时", "キロメートル毎時", false},
        {"°C",   "degree Celsius",     "degrees Celsius",     "摄氏度",     "度",             false},
        {"℃",    "degree Celsius",     "degrees Celsius",     "摄氏度",     "度",             false},
        {"°F",   "degree Fahrenheit",  "degrees Fahrenheit",  "华氏度",     "華氏度",         false},
        {"°",    "degree",             "degrees",             "度",         "度",             true },
        {"mph",  "mile per hour",      "miles per hour",      "英里每小时", "マイル毎時",     false},
        {"kph",  "kilometer per hour", "kilometers per hour", "千米每小时", "キロメートル毎時", false},
        {"km",   "kilometer",          "kilometers",          "千米",       "キロメートル",   false},
        {"cm",   "centimeter",         "centimeters",         "厘米",       "センチメートル", false},
        {"mm",   "millimeter",         "millimeters",         "毫米",       "ミリメートル",   false},
        {"kg",   "kilogram",           "kilograms",           "千克",       "キログラム",     false},
        {"mg",   "milligram",          "milligrams",          "毫克",       "ミリグラム",     false},
        {"ml",   "milliliter",         "milliliters",         "毫升",       "ミリリットル",   false},
        {"mL",   "milliliter",         "milliliters",         "毫升",       "ミリリットル",   false},
        {"lbs",  "pound",              "pounds",              "磅",         "ポンド",         false},
        {"lb",   "pound",              "pounds",              "磅",         "ポンド",         false},
        {"oz",   "ounce",              "ounces",              "盎司",       "オンス",         false},
        {"ft",   "foot",               "feet",                "英尺",       "フィート",       false},
        {"GHz",  "gigahertz",          "gigahertz",           "吉赫",       "ギガヘルツ",     false},
        {"MHz",  "megahertz",          "megahertz",           "兆赫",       "メガヘルツ",     false},
        {"kHz",  "kilohertz",          "kilohertz",           "千赫",       "キロヘルツ",     false},
        {"Hz",   "hertz",              "hertz",               "赫兹",       "ヘルツ",         false},
        {"TB",   "terabyte",           "terabytes",           "TB",         "テラバイト",     false},
        {"GB",   "gigabyte",           "gigabytes",           "GB",         "ギガバイト",     false},
        {"MB",   "megabyte",           "megabytes",           "MB",         "メガバイト",     false},
        {"KB",   "kilobyte",           "kilobytes",           "KB",         "キロバイト",     false},
        {"kB",   "kilobyte",           "kilobytes",           "KB",         "キロバイト",     false},
        {"ms",   "millisecond",        "milliseconds",        "毫秒",       "ミリ秒",         true },
        {"min",  "minute",             "minutes",             "分钟",       "分",             true },
        {"hrs",  "hour",               "hours",               "小时",       "時間",           true },
        {"hr",   "hour",               "hours",               "小时",       "時間",           true },
        {"m",    "meter",              "meters",              "米",         "メートル",       true },
        {"g",    "gram",               "grams",               "克",         "グラム",         true },
    };
    for (int spaced = 0; spaced < 2; ++spaced) {
        const size_t s = k + spaced;
        if (spaced && !(k < t.size() && t[k] == ' ')) break;
        for (const Unit& u : units) {
            if (spaced && u.attachedOnly) continue;
            const U sym = toU32(u.sym);
            if (!startsWith(t, s, sym)) continue;
            const size_t e = s + sym.size();
            if (letterAt(t, e) || (e < t.size() && isAsciiDigit(t[e]))) continue;
            used = e - k;
            return u;
        }
    }
    return std::nullopt;
}

struct Currency { char32_t sym; const char* en1; const char* enN; const char* sub1; const char* subN;
                  const char* zh; const char* ja; };


std::optional<Currency> currencyAt(const U& t, size_t k) {
    if (k >= t.size()) return std::nullopt;
    const Currency currencies[] = {
        {'$',    "dollar", "dollars", "cent",  "cents", "美元", "ドル"},
        {0x20AC, "euro",   "euros",   "cent",  "cents", "欧元", "ユーロ"},
        {0x00A3, "pound",  "pounds",  "penny", "pence", "英镑", "ポンド"},
        {0x00A5, "yen",    "yen",     nullptr, nullptr, "元",   "円"},
        {0xFFE5, "yen",    "yen",     nullptr, nullptr, "元",   "円"},
        {0x20B9, "rupee",  "rupees",  nullptr, nullptr, "卢比", "ルピー"},
    };
    for (const Currency& c : currencies) if (t[k] == c.sym) return c;
    return std::nullopt;
}

// --- Spoken forms of URLs, emails and file names ------------------------------


bool knownSuffix(const std::string& s) {
    const char* const suffixes[] = {
        "com", "org", "net", "io", "ai", "dev", "app", "co", "edu", "gov", "cn", "jp", "uk", "de", "fr",
        "js", "ts", "jsx", "tsx", "json", "py", "md", "txt", "map", "html", "htm", "css", "cpp", "hpp", "cc",
        "h", "c", "java", "kt", "xml", "yml", "yaml", "toml", "csv", "pdf", "png", "jpg", "jpeg", "gif",
        "svg", "zip", "jar", "exe", "sh", "env", "gitignore", "npmrc", "rs", "go", "rb", "php", "sql",
        "wav", "mp3", "mp4", "onnx", "gguf", "cmake", "lock", "log", "ini", "cfg",
    };
    for (const char* k : suffixes) if (s == k) return true;
    return false;
}

struct SymbolWords { const char* dot; const char* slash; const char* at; const char* dash; const char* colon; };

SymbolWords symbolWords(Lang lang) {
    switch (lang) {
    case Lang::Zh: return {"点", "斜杠", "艾特", "杠", "冒号"};
    case Lang::Ja: return {"ドット", "スラッシュ", "アット", "ハイフン", "コロン"};
    default:       return {"dot", "slash", "at", "dash", "colon"};
    }
}

U digitsSpoken(const std::string& d, Lang lang) {
    return lang == Lang::En ? toU32(enDigits(d)) : zhDigits(d, lang == Lang::Ja);
}

// Spell a URL / email / file name piecewise: separators as words, digits one by one.
U spellToken(const U& tok, Lang lang) {
    const SymbolWords w = symbolWords(lang);
    U out;
    auto word = [&](const U& s) {
        if (!out.empty() && out.back() != ' ') out += ' ';
        out += s;
        out += ' ';
    };
    for (size_t i = 0; i < tok.size();) {
        const char32_t c = tok[i];
        if (isAsciiDigit(c)) {
            const size_t e = digitEnd(tok, i);
            word(digitsSpoken(ascii(tok, i, e), lang));
            i = e;
            continue;
        }
        switch (c) {
        case '.': word(toU32(w.dot));   break;
        case '/': word(toU32(w.slash)); break;
        case '@': word(toU32(w.at));    break;
        case '-': word(toU32(w.dash));  break;
        case ':': word(toU32(w.colon)); break;
        case '_': case '+': case '?': case '=': case '&': case '#': case '%': case '~':
            if (!out.empty() && out.back() != ' ') out += ' ';
            break;
        default: out += c;
        }
        ++i;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool urlChar(char32_t c) { return c > 0x20 && c < 0x7F && !inStr(c, U"\"'<>()[]{}|\\^`,"); }

// http(s)://..., www....; trailing sentence punctuation is not part of the URL.
size_t spokenUrlAt(const U& t, size_t i) {
    if (i > 0 && isAsciiWord(t[i - 1])) return 0;
    size_t s = startsWith(t, i, U"https://") ? 8 : startsWith(t, i, U"http://") ? 7 : 0;
    if (!s && !startsWith(t, i, U"www.")) return 0;
    size_t e = i + s;
    while (e < t.size() && urlChar(t[e])) ++e;
    while (e > i + s && inStr(t[e - 1], U".,;:!?")) --e;
    return e > i + s + 1 ? e - i : 0;
}

size_t spokenFileAt(const U& t, size_t i) {
    if (i > 0 && (isAsciiWord(t[i - 1]) || t[i - 1] == '.')) return 0;
    size_t e = i;
    while (e < t.size() && (isAsciiAlnum(t[e]) || inStr(t[e], U"._-"))) ++e;
    while (e > i && inStr(t[e - 1], U".-_")) --e;
    if (e < t.size() && (t[e] == '@' || t[e] == '/')) return 0;
    const U tok = t.substr(i, e - i);
    const size_t dot = tok.rfind(U'.');
    if (dot == U::npos || dot + 1 >= tok.size()) return 0;
    const std::string ext = lowerAscii(tok, dot + 1, tok.size());
    if (!knownSuffix(ext)) return 0;
    // A leading-dot file (.env) or a name with a letter before the dot.
    bool letter = false;
    for (size_t k = 0; k < dot; ++k) letter |= isAsciiLetter(tok[k]);
    if (dot != 0 && !letter) return 0;
    // Keep ordinary sentence ends ("in Japan.Next") out: the suffix must not be capitalised.
    if (isAsciiLetter(tok[dot + 1]) && tok[dot + 1] <= 'Z') return 0;
    return e - i;
}

// --- Abbreviations (English) ----------------------------------------------------

struct Abbrev { const char* from; const char* to; };


// --- The scanner ------------------------------------------------------------------

class Expander {
public:
    Expander(const U& text, Lang lang) : t_(text), lang_(lang), ja_(lang == Lang::Ja) {}

    U run() {
        for (size_t i = 0; i < t_.size();) {
            size_t used = 0;
            U rep;
            if (word(i, used, rep) || number(i, used, rep)) {
                put(rep, i + used);
                i += used;
                continue;
            }
            out_ += t_[i++];
        }
        return out_;
    }

private:
    const U& t_;
    Lang     lang_;
    bool     ja_;
    U        out_;

    bool en() const { return lang_ == Lang::En; }

    // Append a spoken form, keeping English words apart from neighbouring letters/digits.
    void put(const U& rep, size_t next) {
        if (rep.empty()) return;
        if (en()) {
            if (!out_.empty() && isAsciiAlnum(out_.back()) && rep.front() != ' ')
                out_ += ' ';
            out_ += rep;
            if (rep.back() != ' ' && next < t_.size() && (isAsciiAlnum(t_[next]) || t_[next] == '(')) out_ += ' ';
        } else {
            out_ += rep;
        }
    }

    U intWords(const std::string& d) const { return en() ? toU32(enInt(d)) : zhInt(d, ja_); }
    U decWords(const std::string& ip, const std::string& fp) const {
        if (fp.empty()) return intWords(ip);
        return en() ? toU32(enDecimal(ip, fp)) : zhDecimal(ip, fp, ja_);
    }
    U numWords(const Num& n) const {
        if (n.fp.empty() && !n.comma && n.ip.size() > 1 && n.ip[0] == '0') return digitsSpoken(n.ip, lang_);
        return decWords(n.ip, n.fp);
    }
    bool isOne(const Num& n) const { return n.fp.empty() && n.ip == "1"; }

    // --- words: URLs, emails, file names, abbreviations, symbols ---
    bool word(size_t i, size_t& used, U& rep) {
        const char32_t c = t_[i];
        if (const size_t n = spokenUrlAt(t_, i)) {
            size_t s = startsWith(t_, i, U"https://") ? 8 : startsWith(t_, i, U"http://") ? 7 : 0;
            U body = t_.substr(i + s, n - s);
            while (!body.empty() && body.back() == '/') body.pop_back();
            rep = spellToken(body, lang_);
            used = n;
            return true;
        }
        if (const size_t n = matchEmail(t_, i)) {
            rep = spellToken(t_.substr(i, n), lang_);
            used = n;
            return true;
        }
        if (isAsciiAlnum(c) || c == '.') {
            if (const size_t n = spokenFileAt(t_, i)) {
                rep = spellToken(t_.substr(i, n), lang_);
                used = n;
                return true;
            }
        }
        if (en() && (i == 0 || !isAsciiAlnum(t_[i - 1]))) {
            const Abbrev abbrevs[] = {
                {"Mr.", "Mister"}, {"Mrs.", "Missus"}, {"Ms.", "Miz"}, {"Dr.", "Doctor"}, {"Prof.", "Professor"},
                {"Sr.", "Senior"}, {"Jr.", "Junior"}, {"vs.", "versus"}, {"vs", "versus"}, {"e.g.", "for example"},
                {"i.e.", "that is"}, {"etc.", "et cetera"}, {"approx.", "approximately"}, {"a.m.", "a m"},
                {"p.m.", "p m"}, {"Q&A", "Q and A"}, {"w/o", "without"}, {"w/", "with"},
            };
            for (const Abbrev& a : abbrevs) {
                const U from = toU32(a.from);
                if (!startsWith(t_, i, from) || letterAt(t_, i + from.size())) continue;
                rep = toU32(a.to);
                used = from.size();
                // "etc." closing a sentence keeps its full stop.
                if (std::strcmp(a.from, "etc.") == 0) {
                    size_t k = i + used;
                    while (k < t_.size() && t_[k] == ' ') ++k;
                    if (k >= t_.size() || (t_[k] >= 'A' && t_[k] <= 'Z') || !isAsciiAlnum(t_[k])) rep += '.';
                }
                return true;
            }
            if (startsWith(t_, i, U"No.") && i + 4 < t_.size() && t_[i + 3] == ' ' && isAsciiDigit(t_[i + 4])) {
                rep = U"number";
                used = 3;
                return true;
            }
        }
        if (c == '&' && en()) { rep = U" and "; used = 1; return true; }
        if (c == '&')         { rep = ja_ ? U"アンド" : U"和"; used = 1; return true; }
        if (c == '#' && i + 1 < t_.size() && isAsciiDigit(t_[i + 1]) && (i == 0 || !isAsciiWord(t_[i - 1]))) {
            rep = en() ? U"number " : ja_ ? U"番号" : U"第";
            used = 1;
            return true;
        }
        if (c == '@' && i + 1 < t_.size() && isAsciiWord(t_[i + 1]) && (i == 0 || !isAsciiWord(t_[i - 1]))) {
            rep = toU32(symbolWords(lang_).at) + U" ";
            used = 1;
            return true;
        }
        // Arithmetic between numbers.
        if ((c == '+' || c == '=' || c == 0xD7) && betweenNumbers(i)) {
            if (c == '+')  rep = en() ? U"plus"   : ja_ ? U"たす"   : U"加";
            if (c == '=')  rep = en() ? U"equals" : ja_ ? U"イコール" : U"等于";
            if (c == 0xD7) rep = en() ? U"times"  : ja_ ? U"かける" : U"乘";
            if (en()) rep = U" " + rep + U" ";
            used = 1;
            return true;
        }
        return false;
    }

    bool betweenNumbers(size_t i) const {
        size_t a = i, b = i + 1;
        while (a > 0 && t_[a - 1] == ' ') --a;
        while (b < t_.size() && t_[b] == ' ') ++b;
        return a > 0 && isAsciiDigit(t_[a - 1]) && b < t_.size() && (isAsciiDigit(t_[b]) || t_[b] == '-');
    }

    // --- numbers ---
    bool number(size_t i, size_t& used, U& rep) {
        size_t j = i;
        bool neg = false;
        std::optional<Currency> cur;
        if ((t_[j] == '-' || t_[j] == 0x2212) && j + 1 < t_.size() &&
            (isAsciiDigit(t_[j + 1]) || (currencyAt(t_, j + 1) && j + 2 < t_.size() && isAsciiDigit(t_[j + 2])))) {
            const bool boundary = j == 0 || (!isAsciiAlnum(t_[j - 1]) && t_[j - 1] != '-' && t_[j - 1] != '.');
            if (!boundary) return false;
            neg = true;
            ++j;
        }
        if ((cur = currencyAt(t_, j))) {
            if (j + 1 >= t_.size() || !isAsciiDigit(t_[j + 1])) return false;
            ++j;
        }
        if (j >= t_.size() || !isAsciiDigit(t_[j])) return false;
        // Digits glued after a decimal point we did not parse (".5") are left alone.
        if (j == i && i > 0 && t_[i - 1] == '.' && i > 1 && isAsciiDigit(t_[i - 2])) return false;

        if (!neg && !cur) {
            if (patterns(j, used, rep)) { used += j - i; return true; }
        }

        Num n = parseNum(t_, j);
        size_t k = n.end;
        U words;
        const bool plainInt = n.fp.empty() && !n.comma;

        if (cur) {
            words = money(n, *cur, k);
        } else if (k < t_.size() && (t_[k] == '%' || t_[k] == 0x2030)) {
            const bool permille = t_[k] == 0x2030;
            ++k;
            if (en())     words = decWords(n.ip, n.fp) + (permille ? U" per mille" : U" percent");
            else if (ja_) words = decWords(n.ip, n.fp) + (permille ? U"パーミル" : U"パーセント");
            else          words = U"百分之" + decWords(n.ip, n.fp);
            if (!en() && permille) words = U"千分之" + decWords(n.ip, n.fp);
        } else if (plainInt && en() && ordinalSuffixAt(k)) {
            words = toU32(enOrdinalize(enInt(n.ip)));
            k += 2;
        } else if (plainInt && en() && n.ip.size() == 4 && n.ip[3] == '0' && k < t_.size() && t_[k] == 's' &&
                   !letterAt(t_, k + 1)) {
            const int y = std::stoi(n.ip);
            std::string w = (y >= 1000 && y <= 2099) ? enYear(y) : enInt(n.ip);
            if (w.size() > 1 && w.back() == 'y') w = w.substr(0, w.size() - 1) + "ies";
            else w += "s";
            words = toU32(w);
            ++k;
        } else if (plainInt && en() && meridiem(n, k, words)) {
            // handled
        } else if (const auto u = unitAt(t_, k, used)) {
            k += used;
            if (en())     words = decWords(n.ip, n.fp) + U" " + toU32(isOne(n) ? u->en1 : u->enN);
            else          words = decWords(n.ip, n.fp) + toU32(ja_ ? u->ja : u->zh);
        } else if (plainInt && en() && scaleSuffix(n, k, words)) {
            // 5k / 10M / 3B
        } else if (plainInt && isYearContext(n, i)) {
            words = toU32(enYear(std::stoi(n.ip)));
        } else if (plainInt && !en() && k < t_.size() && t_[k] == U'年' && (n.ip.size() == 4 || n.ip.size() == 2)) {
            words = ja_ ? zhInt(n.ip, true) : zhDigits(n.ip, false);
        } else if (plainInt && (n.ip.size() >= 10 || (n.ip.size() > 1 && n.ip[0] == '0'))) {
            words = digitsSpoken(n.ip, lang_);
        } else {
            words = numWords(n);
        }

        if (neg) words = (en() ? U"negative " : ja_ ? U"マイナス" : U"负") + words;
        rep = words;
        used = k - i;
        return true;
    }

    bool ordinalSuffixAt(size_t k) const {
        return wordAt(t_, k, "st") || wordAt(t_, k, "nd") || wordAt(t_, k, "rd") || wordAt(t_, k, "th");
    }

    bool meridiem(const Num& n, size_t& k, U& words) const {
        bool pm = false;
        const size_t m = meridiemAt(t_, k, pm);
        if (!m || n.ip.size() > 2 || std::stoi(n.ip) > 12) return false;
        words = toU32(enInt(n.ip)) + (pm ? U" p m" : U" a m");
        k += m;
        return true;
    }

    bool scaleSuffix(const Num& n, size_t& k, U& words) const {
        if (k >= t_.size() || letterAt(t_, k + 1)) return false;
        const char* scale = t_[k] == 'k' || t_[k] == 'K' ? "thousand" : t_[k] == 'M' ? "million"
                          : t_[k] == 'B' ? "billion" : nullptr;
        if (!scale) return false;
        words = decWords(n.ip, n.fp) + U" " + toU32(scale);
        ++k;
        return true;
    }

    bool isYearContext(const Num& n, size_t i) const {
        if (!en() || n.ip.size() != 4) return false;
        const int y = std::stoi(n.ip);
        if (y < 1000 || y > 2099) return false;
        const char* const contextWords[] = {"in", "since", "by", "from", "year", "until", "till", "of", "before",
                                           "after", "around", "circa", "during", "through", "between", "to",
                                           "early", "late", "mid", "summer", "winter", "spring", "fall", "autumn"};
        const std::string w = prevWord(t_, i);
        for (const char* c : contextWords) if (w == c) return true;
        // "May 2024", "March 3, 2024"
        for (int mi = 1; mi <= 12; ++mi) {
            std::string lm = enMonth(mi);
            for (auto& ch : lm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (w == lm) return true;
        }
        size_t a = i;
        while (a > 0 && t_[a - 1] == ' ') --a;
        if (a > 0 && t_[a - 1] == ',') {
            size_t b = a - 1;
            while (b > 0 && isAsciiDigit(t_[b - 1])) --b;
            for (int mi = 1; mi <= 12; ++mi) {
                std::string lm = enMonth(mi);
                for (auto& ch : lm) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (prevWord(t_, b) == lm) return true;
            }
        }
        return false;
    }

    U money(const Num& n, const Currency& c, size_t& k) const {
        // "$2.5 billion", "$5M"
        const char* scale = nullptr;
        if (en()) {
            for (const char* s : {"thousand", "million", "billion", "trillion"}) {
                if (k < t_.size() && t_[k] == ' ' && wordAt(t_, k + 1, s)) { scale = s; k += 1 + std::strlen(s); break; }
            }
            if (!scale && k < t_.size() && !letterAt(t_, k + 1)) {
                if (t_[k] == 'k' || t_[k] == 'K') { scale = "thousand"; ++k; }
                else if (t_[k] == 'M')           { scale = "million";  ++k; }
                else if (t_[k] == 'B')           { scale = "billion";  ++k; }
            }
            if (scale) return decWords(n.ip, n.fp) + U" " + toU32(scale) + U" " + toU32(c.enN);
            const bool cents = n.fp.size() == 2 && c.sub1;
            if (cents) {
                const int sub = std::stoi(n.fp);
                const std::string subWords = enInt(n.fp[0] == '0' ? n.fp.substr(1) : n.fp) + " " +
                                             (sub == 1 ? c.sub1 : c.subN);
                if (n.ip == "0") return toU32(subWords);
                U w = intWords(n.ip) + U" " + toU32(n.ip == "1" ? c.en1 : c.enN);
                if (sub) w += U" " + toU32(subWords);
                return w;
            }
            return decWords(n.ip, n.fp) + U" " + toU32(isOne(n) ? c.en1 : c.enN);
        }
        return decWords(n.ip, n.fp) + toU32(ja_ ? c.ja : c.zh);
    }

    // Dates, times, phone numbers, ranges, versions, fractions starting at digit j.
    bool patterns(size_t j, size_t& used, U& rep) const {
        const size_t d1 = digitEnd(t_, j);
        const size_t l1 = d1 - j;
        auto num2 = [&](size_t a, size_t b) { return std::stoi(ascii(t_, a, b)); };

        // YYYY-MM-DD, YYYY/MM/DD, YYYY.MM.DD
        if (l1 == 4 && d1 < t_.size() && inStr(t_[d1], U"-/.")) {
            const char32_t sep = t_[d1];
            const size_t d2 = digitEnd(t_, d1 + 1);
            if (d2 - d1 - 1 >= 1 && d2 - d1 - 1 <= 2 && d2 < t_.size() && t_[d2] == sep) {
                const size_t d3 = digitEnd(t_, d2 + 1);
                if (d3 - d2 - 1 >= 1 && d3 - d2 - 1 <= 2 && !(d3 < t_.size() && (t_[d3] == sep || isAsciiDigit(t_[d3])))) {
                    const int y = num2(j, d1), m = num2(d1 + 1, d2), d = num2(d2 + 1, d3);
                    if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                        rep = date(y, m, d, ascii(t_, j, d1));
                        used = d3 - j;
                        return true;
                    }
                }
            }
        }
        // M/D/YYYY (English)
        if (en() && l1 <= 2 && d1 < t_.size() && t_[d1] == '/') {
            const size_t d2 = digitEnd(t_, d1 + 1);
            if (d2 - d1 - 1 >= 1 && d2 - d1 - 1 <= 2 && d2 < t_.size() && t_[d2] == '/') {
                const size_t d3 = digitEnd(t_, d2 + 1);
                if (d3 - d2 - 1 == 4) {
                    const int m = num2(j, d1), d = num2(d1 + 1, d2), y = num2(d2 + 1, d3);
                    if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                        rep = date(y, m, d, ascii(t_, d2 + 1, d3));
                        used = d3 - j;
                        return true;
                    }
                }
            }
        }
        // H:MM(:SS)
        if (l1 <= 2 && d1 < t_.size() && t_[d1] == ':' && digitEnd(t_, d1 + 1) == d1 + 3) {
            size_t e = d1 + 3;
            int s = -1;
            if (e < t_.size() && t_[e] == ':' && digitEnd(t_, e + 1) == e + 3) {
                s = num2(e + 1, e + 3);
                e += 3;
            }
            if (!(e < t_.size() && isAsciiDigit(t_[e]))) {
                const int h = num2(j, d1), m = num2(d1 + 1, d1 + 3);
                if (h <= 24 && m <= 59 && s <= 59) {
                    rep = time(h, m, s, e);
                    used = e - j;
                    return true;
                }
            }
        }
        // NNN-NNN-NNNN phone numbers
        if (l1 == 3 && d1 < t_.size() && t_[d1] == '-') {
            const size_t d2 = digitEnd(t_, d1 + 1);
            if (d2 - d1 - 1 == 3 && d2 < t_.size() && t_[d2] == '-') {
                const size_t d3 = digitEnd(t_, d2 + 1);
                if (d3 - d2 - 1 == 4) {
                    const U sep = en() ? U", " : U"，";
                    rep = digitsSpoken(ascii(t_, j, d1), lang_) + sep + digitsSpoken(ascii(t_, d1 + 1, d2), lang_) +
                          sep + digitsSpoken(ascii(t_, d2 + 1, d3), lang_);
                    used = d3 - j;
                    return true;
                }
            }
        }
        // Versions: 1.2.3
        {
            size_t e = d1, parts = 1;
            while (e + 1 < t_.size() && t_[e] == '.' && isAsciiDigit(t_[e + 1])) { e = digitEnd(t_, e + 1); ++parts; }
            if (parts >= 3) {
                U w;
                size_t a = j;
                while (a < e) {
                    const size_t b = digitEnd(t_, a);
                    if (!w.empty()) w += en() ? U" point " : U"点";
                    w += intWords(ascii(t_, a, b));
                    a = b + 1;
                }
                rep = w;
                used = e - j;
                return true;
            }
        }
        // Fractions a/b (not dates, not paths)
        if (l1 <= 3 && d1 + 1 < t_.size() && t_[d1] == '/' && isAsciiDigit(t_[d1 + 1])) {
            const size_t d2 = digitEnd(t_, d1 + 1);
            if (d2 - d1 - 1 <= 3 && !(d2 < t_.size() && (t_[d2] == '/' || t_[d2] == '.'))) {
                const std::string a = ascii(t_, j, d1), b = ascii(t_, d1 + 1, d2);
                if (std::stoi(b) > 1) {
                    rep = fraction(a, b);
                    used = d2 - j;
                    return true;
                }
            }
        }
        // Ranges a-b / a~b
        if (d1 < t_.size() && (t_[d1] == '-' || t_[d1] == '~' || t_[d1] == 0xFF5E || t_[d1] == 0x301C ||
                               t_[d1] == 0x2013)) {
            size_t b0 = d1 + 1;
            if (b0 < t_.size() && isAsciiDigit(t_[b0])) {
                const Num a = parseNum(t_, j);
                if (a.end == d1) {
                    const Num b = parseNum(t_, b0);
                    if (!(b.end < t_.size() && (t_[b.end] == '-' || isAsciiDigit(t_[b.end])))) {
                        const bool years = en() && a.ip.size() == 4 && b.ip.size() == 4 && a.fp.empty() &&
                                           b.fp.empty() && std::stoi(a.ip) >= 1000 && std::stoi(b.ip) <= 2099;
                        const U wa = years ? toU32(enYear(std::stoi(a.ip))) : numWords(a);
                        const U wb = years ? toU32(enYear(std::stoi(b.ip))) : numWords(b);
                        rep = wa + (en() ? U" to " : ja_ ? U"から" : U"到") + wb;
                        used = b.end - j;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    U date(int y, int m, int d, const std::string& yearDigits) const {
        if (en()) return toU32(std::string(enMonth(m)) + " " + enOrdinalize(enCardinal(d)) + ", " + enYear(y));
        if (ja_)  return zhCardinal(y, true) + U"年" + zhCardinal(m, true) + U"月" + zhCardinal(d, true) + U"日";
        return zhDigits(yearDigits, false) + U"年" + zhCardinal(m, false) + U"月" + zhCardinal(d, false) + U"日";
    }

    U time(int h, int m, int s, size_t& end) const {
        if (en()) {
            bool pm = false;
            const size_t mer = meridiemAt(t_, end, pm);
            std::string w = enCardinal(h);
            if (m == 0)      w += mer ? "" : " o'clock";
            else if (m < 10) w += std::string(" oh ") + enOnes(m);
            else             w += " " + enBelow100(m);
            if (s > 0) w += " and " + enCardinal(s) + (s == 1 ? " second" : " seconds");
            if (mer) { w += pm ? " p m" : " a m"; end += mer; }
            return toU32(w);
        }
        if (ja_) {
            U w = zhCardinal(h, true) + U"時";
            if (m) w += zhCardinal(m, true) + U"分";
            if (s > 0) w += zhCardinal(s, true) + U"秒";
            return w;
        }
        U w = zhCardinal(h, false) + U"点";
        if (m) w += (m < 10 ? U"零" : U"") + zhCardinal(m, false) + U"分";
        if (s > 0) w += zhCardinal(s, false) + U"秒";
        return w;
    }

    U fraction(const std::string& a, const std::string& b) const {
        if (!en()) return intWords(b) + (ja_ ? U"分の" : U"分之") + intWords(a);
        if (a == "24" && b == "7") return U"twenty four seven";
        const bool plural = a != "1";
        std::string den;
        if (b == "2")      den = plural ? "halves" : "half";
        else if (b == "4") den = plural ? "quarters" : "quarter";
        else               den = enOrdinalize(enInt(b)) + (plural ? "s" : "");
        return toU32(enInt(a) + " " + den);
    }
};

// Fullwidth digits / signs → ASCII so the scanner sees one form.
U foldWidth(const U& in) {
    U t = in;
    for (size_t i = 0; i < t.size(); ++i) {
        char32_t& c = t[i];
        if (c >= 0xFF10 && c <= 0xFF19) c = '0' + (c - 0xFF10);
        else if (c == 0xFF05) c = '%';
        else if (c == 0xFF0B) c = '+';
        else if (c == 0xFF1D) c = '=';
        else if (c == 0xFF04) c = '$';
    }
    // ．：－ between digits
    for (size_t i = 1; i + 1 < t.size(); ++i) {
        if (!isAsciiDigit(t[i - 1]) || !isAsciiDigit(t[i + 1])) continue;
        if (t[i] == 0xFF0E) t[i] = '.';
        else if (t[i] == 0xFF1A) t[i] = ':';
        else if (t[i] == 0xFF0D) t[i] = '-';
    }
    return t;
}

// Markdown emphasis / code markers, bullets and emoji, which the robust pass keeps.
std::string stripDecorations(const std::string& text) {
    const U t = toU32(text);
    U out;
    bool lineStart = true;
    for (size_t i = 0; i < t.size(); ++i) {
        const char32_t c = t[i];
        if (c == '\n') { out += c; lineStart = true; continue; }
        if (c == '`') continue;
        if (c == '*') {
            const bool arithmetic = i > 0 && i + 1 < t.size() && isAsciiDigit(t[i - 1]) &&
                                    (isAsciiDigit(t[i + 1]) || t[i + 1] == ' ');
            if (!arithmetic && !(lineStart && i + 1 < t.size() && t[i + 1] == ' ')) continue;
        }
        if (lineStart && (c == 0x2022 || c == 0x25CF || c == 0x25AA || c == 0x2023)) {
            out += U"- ";
            if (i + 1 < t.size() && t[i + 1] == ' ') ++i;
            lineStart = false;
            continue;
        }
        const bool emoji = (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) ||
                           c == 0xFE0F || c == 0x200D || (c >= 0x1F1E6 && c <= 0x1F1FF) ||
                           (c >= 0x2B00 && c <= 0x2BFF);
        if (emoji) continue;
        if (!isWs(c)) lineStart = false;
        out += c;
    }
    return toUtf8(out);
}

} // namespace

std::string expand(const std::string& text, Lang lang) {
    const U t = foldWidth(toU32(text));
    return toUtf8(Expander(t, lang).run());
}

std::string normalize(const std::string& text, bool englishVoice) {
    std::string t = robustNormalize(stripDecorations(text));
    if (t.empty()) return t;
    const Lang lang = detectLanguage(t, englishVoice);
    if (lang != Lang::En) t = rewriteHyphensForZh(t);
    t = expand(t, lang);
    return robustNormalize(t);
}

} // namespace moss::text
