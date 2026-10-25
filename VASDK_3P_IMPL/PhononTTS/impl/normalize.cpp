// normalize.cpp — C++ port of gradium-ai/xn-ptts v0.4.1 `ptts::preprocess`.
//
// Layout follows the Rust sources so the two can be read side by side: the character pass
// (preprocess.rs: StringAppender, normalize_text, spell_symbols, split_slash), then the word
// rules (preprocess/rewrite.rs and one section per rewrite/*.rs module).
//
// Conventions that keep the port exact:
//  * The character pass works on code points (std::u32string), as Rust's `chars()` does.
//  * The rewrite rules work on UTF-8 bytes (std::string_view), as Rust's `&str` does: every
//    `len()` there is a byte length, and every slice index a byte index.
//  * Unicode properties (`is_alphabetic`, `is_numeric`, `is_uppercase`, `is_lowercase`) come
//    from tables generated from Rust's core::char; `is_whitespace` and `is_control` are small
//    fixed sets, spelled out below.

#include "normalize.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gradium::tts::text {

namespace {

// ===========================================================================================
// Unicode
// ===========================================================================================

struct CodeRange { char32_t lo, hi; };
struct UpperToAscii { char32_t cp; const char* upper; };
struct UpperMapping { char32_t cp; char32_t up[3]; };
struct FoldPair { char32_t from; char to; };

#include "unicode_props.inc"
#include "normalize_tables.inc"

template <size_t N>
bool inRanges(const CodeRange (&table)[N], char32_t c) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < table[mid].lo) hi = mid;
        else if (c > table[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

bool isAlphabetic(char32_t c) { return inRanges(kAlphabetic, c); }
bool isNumeric(char32_t c) { return inRanges(kNumeric, c); }
bool isAlphanumeric(char32_t c) { return isAlphabetic(c) || isNumeric(c); }
bool isUppercase(char32_t c) { return inRanges(kUppercase, c); }
bool isLowercase(char32_t c) { return inRanges(kLowercase, c); }

// Rust's char::is_whitespace: the White_Space property.
bool isWhitespace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000;
}

// Rust's char::is_control: general category Cc.
bool isControl(char32_t c) { return c <= 0x1F || (c >= 0x7F && c <= 0x9F); }

bool isAsciiDigit(char32_t c) { return c >= '0' && c <= '9'; }
bool isAsciiUpper(char32_t c) { return c >= 'A' && c <= 'Z'; }
bool isAsciiLower(char32_t c) { return c >= 'a' && c <= 'z'; }
bool isAsciiAlpha(char32_t c) { return isAsciiUpper(c) || isAsciiLower(c); }
bool isAsciiAlnum(char32_t c) { return isAsciiAlpha(c) || isAsciiDigit(c); }
bool isAsciiPunct(char32_t c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}
char asciiUpper(char c) { return (c >= 'a' && c <= 'z') ? char(c - 32) : c; }
char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

// ===========================================================================================
// UTF-8
// ===========================================================================================

// Decode one code point at byte `i`; invalid sequences decode to U+FFFD one byte at a time,
// as String::from_utf8_lossy would (the reference only ever sees valid UTF-8).
char32_t decodeAt(std::string_view s, size_t i, size_t* len) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    if (b0 < 0x80) { *len = 1; return b0; }
    if ((b0 & 0xE0) == 0xC0 && b0 >= 0xC2 && cont(1)) {
        *len = 2;
        return char32_t(((b0 & 0x1F) << 6) | (s[i + 1] & 0x3F));
    }
    if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        char32_t c = ((b0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        if (c >= 0x800 && (c < 0xD800 || c > 0xDFFF)) { *len = 3; return c; }
    }
    if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        char32_t c = ((b0 & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) |
                     (s[i + 3] & 0x3F);
        if (c >= 0x10000 && c <= 0x10FFFF) { *len = 4; return c; }
    }
    *len = 1;
    return 0xFFFD;
}

std::u32string decode(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t n;
        out.push_back(decodeAt(s, i, &n));
        i += n;
    }
    return out;
}

void encodeTo(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string encode(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) encodeTo(out, c);
    return out;
}

// The last code point of `s` and its byte length; 0 for an empty string.
char32_t lastChar(std::string_view s, size_t* len) {
    if (s.empty()) { *len = 0; return 0; }
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80 && s.size() - i < 4) --i;
    size_t n;
    char32_t c = decodeAt(s, i, &n);
    if (i + n != s.size()) { *len = 1; return 0xFFFD; }
    *len = n;
    return c;
}

char32_t firstChar(std::string_view s, size_t* len) {
    if (s.empty()) { *len = 0; return 0; }
    return decodeAt(s, 0, len);
}

size_t charCount(std::string_view s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        size_t k;
        decodeAt(s, i, &k);
        i += k;
        ++n;
    }
    return n;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool endsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

bool parseU32(std::string_view s, uint32_t* out) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + uint64_t(c - '0');
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = uint32_t(v);
    return true;
}

// ===========================================================================================
// Spoken forms (preprocess.rs: SpecialChars and Lang's methods)
// ===========================================================================================

struct SpecialChars {
    const char* at;
    const char* plus;
    const char* equals;
    const char* colon;
    const char* slash;
    const char* dash;
    const char* dot;
    const char* underscore;
};

const SpecialChars kSpecialEn{"at", "plus", "equals", "colon", "slash", "dash", "dot", "underscore"};
const SpecialChars kSpecialFr{"arobaze", "plus", "égal", "deux-points", "slash", "tiret", "point", "underscore"};
const SpecialChars kSpecialDe{"ät", "Plus", "Gleich", "Doppelpunkt", "Slash", "Bindestrich", "Punkt", "Unterstrich"};
const SpecialChars kSpecialEs{"arroba", "mas", "igual", "dos-puntos", "slash", "guion", "punto", "guion bajo"};
const SpecialChars kSpecialPt{"arroba", "mais", "igual", "dois-pontos", "slash", "hifen", "ponto", "underscore"};

const SpecialChars& special(Lang lang) {
    switch (lang) {
    case Lang::En: return kSpecialEn;
    case Lang::Fr: return kSpecialFr;
    case Lang::De: return kSpecialDe;
    case Lang::Es: return kSpecialEs;
    case Lang::Pt: return kSpecialPt;
    }
    return kSpecialEn;
}

const char* about(Lang lang) {
    switch (lang) {
    case Lang::En: return "about";
    case Lang::Fr: return "environ";
    case Lang::De: return "etwa";
    default: return "aproximadamente";
    }
}

const char* plusMinus(Lang lang) {
    switch (lang) {
    case Lang::En: return "plus or minus";
    case Lang::Fr: return "plus ou moins";
    case Lang::De: return "plus minus";
    case Lang::Es: return "más o menos";
    case Lang::Pt: return "mais ou menos";
    }
    return "";
}

const char* piWord(Lang lang) { return lang == Lang::De ? "Pi" : "pi"; }

const char* love(Lang lang) {
    switch (lang) {
    case Lang::En: return "love";
    case Lang::Fr: return "aime";
    case Lang::De: return "liebe";
    default: return "amo";
    }
}

const char* star(Lang lang) {
    switch (lang) {
    case Lang::En: return "star";
    case Lang::Fr: return "étoile";
    case Lang::De: return "Stern";
    default: return "asterisco";
    }
}

const char* numberSign(Lang lang) {
    switch (lang) {
    case Lang::En: return "number";
    case Lang::Fr: return "numéro";
    case Lang::De: return "Nummer";
    default: return "número";
    }
}

const char* degrees(Lang lang, bool one) {
    switch (lang) {
    case Lang::En: return one ? "degree" : "degrees";
    case Lang::Fr: return one ? "degré" : "degrés";
    case Lang::De: return "Grad";
    case Lang::Es: return one ? "grado" : "grados";
    case Lang::Pt: return one ? "grau" : "graus";
    }
    return "";
}

const char* symbolWord(Lang lang, char32_t c) {
    const int l = int(lang); // En, Fr, De, Es, Pt
    auto pick = [&](const char* en, const char* fr, const char* de, const char* es, const char* pt) {
        const char* w[5] = {en, fr, de, es, pt};
        return w[l];
    };
    switch (c) {
    case U'×': return pick("times", "fois", "mal", "por", "vezes");
    case U'÷': return pick("divided by", "divisé par", "geteilt durch", "dividido entre", "dividido por");
    case U'≤': return pick("less than or equal to", "inférieur ou égal à", "kleiner oder gleich", "menor o igual que", "menor ou igual a");
    case U'≥': return pick("greater than or equal to", "supérieur ou égal à", "größer oder gleich", "mayor o igual que", "maior ou igual a");
    case U'≠': return pick("is not equal to", "différent de", "ungleich", "distinto de", "diferente de");
    case U'≈': return pick("approximately", "environ", "ungefähr", "aproximadamente", "aproximadamente");
    case U'√': return pick("square root of", "racine carrée de", "Wurzel aus", "raíz cuadrada de", "raiz quadrada de");
    case U'∞': return pick("infinity", "infini", "unendlich", "infinito", "infinito");
    case U'∑': return pick("sigma", "sigma", "Sigma", "sigma", "sigma");
    case U'‰': return pick("per mille", "pour mille", "Promille", "por mil", "por mil");
    default: return nullptr;
    }
}

std::string stars(Lang lang, const std::string& n, std::optional<size_t> of) {
    if (of) {
        const std::string o = std::to_string(*of);
        switch (lang) {
        case Lang::En: return n + " out of " + o + " stars";
        case Lang::Fr: return n + " étoiles sur " + o;
        case Lang::De: return n + " von " + o + " Sternen";
        case Lang::Es: return n + " de " + o + " estrellas";
        case Lang::Pt: return n + " de " + o + " estrelas";
        }
    }
    switch (lang) {
    case Lang::En: return n + " stars";
    case Lang::Fr: return n + " étoiles";
    case Lang::De: return n + " Sterne";
    case Lang::Es: return n + " estrellas";
    case Lang::Pt: return n + " estrelas";
    }
    return n;
}

std::string power(Lang lang, const std::string& n) {
    switch (lang) {
    case Lang::En:
        if (n == "2") return "squared";
        if (n == "3") return "cubed";
        return "to the power of " + n;
    case Lang::Fr:
        if (n == "2") return "au carré";
        if (n == "3") return "au cube";
        return "puissance " + n;
    case Lang::De:
        return "hoch " + n;
    case Lang::Es:
        if (n == "2") return "al cuadrado";
        if (n == "3") return "al cubo";
        return "elevado a " + n;
    case Lang::Pt:
        if (n == "2") return "ao quadrado";
        if (n == "3") return "ao cubo";
        return "elevado a " + n;
    }
    return n;
}

std::optional<std::string> greek(Lang lang, char32_t c) {
    static const char* const kNames[24] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa",
        "lambda", "mu", "nu", "xi", "omicron", "pi", "rho", "sigma", "tau", "upsilon", "phi",
        "chi", "psi", "omega",
    };
    size_t i;
    if (c >= U'α' && c <= U'ρ') i = c - U'α';
    else if (c == U'ς') i = 17;
    else if (c >= U'σ' && c <= U'ω') i = c - U'α' - 1;
    else if (c >= U'Α' && c <= U'Ρ') i = c - U'Α';
    else if (c >= U'Σ' && c <= U'Ω') i = c - U'Α' - 1;
    else return std::nullopt;
    std::string name = kNames[i];
    if (lang == Lang::De) name[0] = asciiUpper(name[0]);
    return name;
}

// `unaccented` in preprocess.rs: the FOLDS table, which has no duplicate keys.
std::optional<char32_t> fold(char32_t c) {
    if (c < 0xCC) return std::nullopt;
    for (const auto& p : kFolds)
        if (p.from == c) return char32_t(p.to);
    return std::nullopt;
}

std::optional<char32_t> subscript(char32_t c) {
    if (c >= U'₀' && c <= U'₉') return char32_t('0' + (c - U'₀'));
    switch (c) {
    case U'₊': return U'+';
    case U'₋': return U'-';
    case U'ₐ': return U'a';
    case U'ₑ': return U'e';
    case U'ₒ': return U'o';
    case U'ₓ': return U'x';
    case U'ₕ': return U'h';
    case U'ₖ': return U'k';
    case U'ₗ': return U'l';
    case U'ₘ': return U'm';
    case U'ₙ': return U'n';
    case U'ₚ': return U'p';
    case U'ₛ': return U's';
    case U'ₜ': return U't';
    case U'ᵢ': return U'i';
    case U'ⱼ': return U'j';
    default: return std::nullopt;
    }
}

std::optional<char32_t> superscript(char32_t c) {
    switch (c) {
    case U'⁰': return U'0';
    case U'¹': return U'1';
    case U'²': return U'2';
    case U'³': return U'3';
    case U'⁻': return U'-';
    case U'⁺': return U'+';
    case U'ⁿ': return U'n';
    default: break;
    }
    if (c >= U'⁴' && c <= U'⁹') return char32_t('4' + (c - U'⁴'));
    return std::nullopt;
}

bool isEmoji(char32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) ||
           (c >= 0x2B00 && c <= 0x2BFF) || c == 0xFE0F || c == 0x200D;
}

bool isDoubleQuote(char32_t c) {
    switch (c) {
    case U'»': case U'«': case U'‹': case U'›': case U'“': case U'”': case U'„': case U'‟':
    case U'″': case U'‶': case U'⹂': case U'❝': case U'❞': case U'❠': case U'〝': case U'〞':
    case U'〟': case U'＂':
        return true;
    default:
        return false;
    }
}

bool isSingleQuote(char32_t c) {
    switch (c) {
    case U'‘': case U'’': case U'‚': case U'‛': case U'′': case U'‵': case U'❛': case U'❜':
    case U'❟': case U'＇': case U'ʼ': case U'ʻ': case U'ʹ': case U'´': case U'`':
        return true;
    default:
        return false;
    }
}

bool isCurrencySymbol(char32_t c) {
    return c == U'$' || c == U'€' || c == U'£' || c == U'¥' || c == U'¢';
}

// ===========================================================================================
// StringAppender (preprocess.rs)
// ===========================================================================================

// Character sink that keeps the output free of the punctuation pile-ups the substitutions
// would otherwise produce: a `.` or `,` swallows any whitespace and punctuation immediately
// before it, and runs of whitespace collapse to a single space.
struct Appender {
    std::u32string b;

    void push(char32_t c) {
        if (c == U'.' || c == U',') {
            while (!b.empty() && (isWhitespace(b.back()) || (isAsciiPunct(b.back()) && !keeps(b.back()))))
                b.pop_back();
        }
        b.push_back(c);
    }

    void pushStr(std::string_view s) {
        for (size_t i = 0; i < s.size();) {
            size_t n;
            push(decodeAt(s, i, &n));
            i += n;
        }
    }

    bool keeps(char32_t last) const {
        switch (last) {
        case U'"': case U'\'': case U'@': case U'+': case U'%': case U'#': case U'&':
            return true;
        case U'$':
            return b.size() >= 2 && isAsciiDigit(b[b.size() - 2]);
        default:
            return false;
        }
    }

    bool endsWithOne() const {
        size_t end = 0; // one past the last non-whitespace character
        for (size_t i = b.size(); i > 0; --i) {
            if (!isWhitespace(b[i - 1])) { end = i; break; }
        }
        if (end == 0 || b[end - 1] != U'1') return false;
        if (end >= 2) {
            char32_t p = b[end - 2];
            if (isAsciiDigit(p) || p == U'.' || p == U',') return false;
        }
        return true;
    }

    bool endsWithN() const {
        size_t len = b.size();
        if (len > 0 && b[len - 1] == U'.') --len;
        if (len == 0 || (b[len - 1] != U'n' && b[len - 1] != U'N')) return false;
        return !(len >= 2 && isAlphanumeric(b[len - 2]));
    }

    bool endsWithSpacedDigit() const {
        if (!lastIsWhitespace()) return false;
        for (size_t i = b.size(); i > 0; --i) {
            if (!isWhitespace(b[i - 1])) return isAsciiDigit(b[i - 1]);
        }
        return false;
    }

    std::optional<size_t> thousandsLead() const {
        size_t n = 0;
        while (n < b.size() && isAsciiDigit(b[b.size() - 1 - n])) ++n;
        if (n < 1 || n > 3) return std::nullopt;
        if (b[b.size() - n] == U'0') return std::nullopt;
        const size_t beforeLen = b.size() - n;
        if (beforeLen > 0) {
            char32_t last = b[beforeLen - 1];
            if (isAlphanumeric(last) || last == U',' || last == U'.') return std::nullopt;
        }
        for (size_t i = beforeLen; i > 0; --i) {
            if (!isWhitespace(b[i - 1])) {
                if (isAsciiDigit(b[i - 1])) return std::nullopt;
                break;
            }
        }
        return n;
    }

    void pushSpoken(std::string_view word) {
        pushWhitespace();
        pushStr(word);
        pushWhitespace();
    }

    bool lastIsWhitespace() const { return !b.empty() && isWhitespace(b.back()); }

    void pushWhitespace() {
        if (!lastIsWhitespace() && !b.empty()) push(U' ');
    }

    void popWhitespace() {
        while (lastIsWhitespace()) b.pop_back();
    }

    std::string intoString() {
        popWhitespace();
        return encode(b);
    }
};

// How many groups of exactly three digits, each but the first after a space, the text from
// `pos` starts with: the rest of a French number. 0 when more digits follow them.
size_t thousandsGroups(const std::u32string& in, size_t pos) {
    const size_t len = std::min<size_t>(64, in.size() - pos);
    const char32_t* ahead = in.data() + pos;
    size_t n = 0, i = 0;
    for (;;) {
        size_t run = 0;
        while (i + run < len && isAsciiDigit(ahead[i + run])) ++run;
        if (run == 0) return n;
        if (run != 3) return 0;
        ++n;
        i += 3;
        if (!(i < len && isWhitespace(ahead[i]))) return n;
        i += 1;
    }
}

// ===========================================================================================
// Rewrite rules: shared helpers (rewrite.rs)
// ===========================================================================================

constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026

// One of the sentence punctuation characters "!?.:,;…" a rule carries through.
bool isPunctuationChar(char32_t c) {
    return c == U'!' || c == U'?' || c == U'.' || c == U':' || c == U',' || c == U';' || c == U'…';
}

// `rest` when it is only sentence punctuation; nullopt otherwise.
std::optional<std::string_view> suffix(std::string_view rest) {
    for (size_t i = 0; i < rest.size();) {
        size_t n;
        if (!isPunctuationChar(decodeAt(rest, i, &n))) return std::nullopt;
        i += n;
    }
    return rest;
}

bool allDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// ===========================================================================================
// numbers.rs
// ===========================================================================================

std::string spell(std::string_view s, Lang lang) {
    static const char* const kWords[5][10] = {
        {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"},
        {"zéro", "un", "deux", "trois", "quatre", "cinq", "six", "sept", "huit", "neuf"},
        {"null", "eins", "zwei", "drei", "vier", "fünf", "sechs", "sieben", "acht", "neun"},
        {"cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve"},
        {"zero", "um", "dois", "três", "quatro", "cinco", "seis", "sete", "oito", "nove"},
    };
    std::vector<std::string> spoken;
    for (size_t i = 0; i < s.size();) {
        size_t n;
        char32_t c = decodeAt(s, i, &n);
        if (c != U'-') {
            if (isAsciiDigit(c)) spoken.emplace_back(kWords[int(lang)][c - U'0']);
            else spoken.emplace_back(s.substr(i, n));
        }
        i += n;
    }
    return join(spoken, " ");
}

std::optional<std::string> range(std::string_view body, Lang lang) {
    const size_t dash = body.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const std::string_view from = body.substr(0, dash), to = body.substr(dash + 1);
    const size_t shortest = lang == Lang::De ? 1 : 2;
    auto small = [&](std::string_view s) {
        return s.size() >= shortest && s.size() <= 3 && allDigits(s) && (s.size() == 1 || s[0] != '0');
    };
    auto year = [](std::string_view s) {
        return s.size() == 4 && allDigits(s) && (startsWith(s, "1") || startsWith(s, "20"));
    };
    bool ok;
    if (small(from) && small(to)) {
        uint32_t a, b;
        if (!parseU32(from, &a) || !parseU32(to, &b)) return std::nullopt;
        ok = a < b;
    } else if (from.size() == 4 && to.size() == 4) {
        ok = year(from) && year(to) && from < to;
    } else if (from.size() == 4 && to.size() == 2) {
        ok = year(from) && allDigits(to) && from.substr(2) < to;
    } else {
        ok = false;
    }
    if (!ok) return std::nullopt;
    const char* word = "";
    switch (lang) {
    case Lang::En: word = "to"; break;
    case Lang::Fr: word = "à"; break;
    case Lang::De: word = "bis"; break;
    default: word = "a"; break;
    }
    return std::string(from) + " " + word + " " + std::string(to);
}

const char* decimalSeparator(Lang lang) {
    switch (lang) {
    case Lang::En: return "point";
    case Lang::Fr: return "virgule";
    case Lang::De: return "Komma";
    case Lang::Es: return "coma";
    case Lang::Pt: return "vírgula";
    }
    return "";
}

// What `numbers` reads `word` as, and the trailing punctuation it carries through.
std::optional<std::pair<std::string, std::string_view>> numberWords(std::string_view word, Lang lang) {
    if (word.size() >= 2 && word[0] == '0' && isAsciiDigit(static_cast<unsigned char>(word[1])))
        return std::nullopt;

    std::optional<char> group;
    char point;
    switch (lang) {
    case Lang::En:
        group = ',';
        point = '.';
        break;
    case Lang::Fr:
        point = ',';
        break;
    default: {
        const size_t comma = word.rfind(',');
        const std::string_view intPart = comma == std::string_view::npos ? word : word.substr(0, comma);
        bool grouped = intPart.find('.') != std::string_view::npos;
        if (grouped) {
            const auto groups = split(intPart, '.');
            for (size_t i = 1; i < groups.size(); ++i)
                if (groups[i].size() != 3) { grouped = false; break; }
        }
        if (grouped) group = '.';
        point = ',';
        break;
    }
    }

    bool negative = false;
    std::string_view rest = word;
    if (!rest.empty() && rest[0] == '-') {
        negative = true;
        rest.remove_prefix(1);
    }
    size_t end = 0;
    while (end < rest.size() && (isAsciiDigit(static_cast<unsigned char>(rest[end])) || (group && rest[end] == *group)))
        ++end;
    std::string_view intDigits = rest.substr(0, end);
    while (group && !intDigits.empty() && intDigits.back() == *group) intDigits.remove_suffix(1);
    if (intDigits.empty()) return std::nullopt;
    rest = rest.substr(intDigits.size());

    std::optional<std::string_view> decimal;
    if (!rest.empty() && rest[0] == point) {
        const std::string_view after = rest.substr(1);
        size_t n = 0;
        while (n < after.size() && isAsciiDigit(static_cast<unsigned char>(after[n]))) ++n;
        if (n != 0) {
            decimal = after.substr(0, n);
            rest = after.substr(n);
        }
    }
    const auto sfx = suffix(rest);
    if (!sfx) return std::nullopt;

    std::string digitStr;
    for (char c : intDigits)
        if (c >= '0' && c <= '9') digitStr += c;
    // `digits.parse::<i64>()`: leading zeros are fine, a value past i64::MAX fails.
    if (digitStr.empty()) return std::nullopt;
    uint64_t uv = 0;
    for (char c : digitStr) {
        const uint64_t d = uint64_t(c - '0');
        if (uv > (9223372036854775807ull - d) / 10) return std::nullopt;
        uv = uv * 10 + d;
    }
    const int64_t value = int64_t(uv);
    if (value >= 1000000000000LL) return std::nullopt;

    std::string words;
    if (value < 1000 || (value >= 1900 && value < 2100)) {
        words = std::to_string(value);
    } else {
        struct Scale { int64_t div; const char* one; const char* many; };
        static const Scale kScales[5][3] = {
            {{1000000000, "billion", "billion"}, {1000000, "million", "million"}, {1000, "thousand", "thousand"}},
            {{1000000000, "milliard", "milliards"}, {1000000, "million", "millions"}, {1000, "mille", "mille"}},
            {{1000000000, "Milliarde", "Milliarden"}, {1000000, "Million", "Millionen"}, {1000, "Tausend", "Tausend"}},
            {{1000000000, "mil millones", "mil millones"}, {1000000, "millón", "millones"}, {1000, "mil", "mil"}},
            {{1000000000, "bilhão", "bilhões"}, {1000000, "milhão", "milhões"}, {1000, "mil", "mil"}},
        };
        std::vector<std::string> parts;
        for (const auto& s : kScales[int(lang)]) {
            const int64_t g = (value / s.div) % 1000;
            if (g == 0) continue;
            if (g == 1) {
                if ((lang == Lang::Fr || lang == Lang::Es || lang == Lang::Pt) && s.div == 1000)
                    parts.emplace_back(s.one);
                else if (lang == Lang::De && s.div == 1000)
                    parts.push_back(std::string("ein ") + s.one);
                else if (lang == Lang::De)
                    parts.push_back(std::string("eine ") + s.one);
                else
                    parts.push_back(std::string("1 ") + s.one);
            } else {
                parts.push_back(std::to_string(g) + " " + s.many);
            }
        }
        if (value % 1000 != 0) parts.push_back(std::to_string(value % 1000));
        words = join(parts, " ");
    }
    if (decimal) words = words + " " + decimalSeparator(lang) + " " + std::string(*decimal);
    if (negative) {
        const char* minus = lang == Lang::Fr ? "moins" : (lang == Lang::Es || lang == Lang::Pt) ? "menos" : "minus";
        words = std::string(minus) + " " + words;
    }
    return std::make_pair(words, *sfx);
}

} // namespace

// ===========================================================================================
// detail: shared helpers exposed for the tests
// ===========================================================================================

namespace detail {

std::pair<std::string_view, std::string_view> splitSuffix(std::string_view word) {
    size_t end = word.size();
    while (end > 0) {
        size_t n;
        const char32_t c = lastChar(word.substr(0, end), &n);
        if (!isPunctuationChar(c)) break;
        end -= n;
    }
    return {word.substr(0, end), word.substr(end)};
}

std::optional<std::pair<std::string_view, std::string_view>> digits(std::string_view s, size_t min, size_t max) {
    size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
    n = std::min(n, max);
    if (n < min) return std::nullopt;
    return std::make_pair(s.substr(0, n), s.substr(n));
}

std::string spellShort(std::string_view word) {
    if (word == "www" || word == "WWW") return "W-W-W";
    if (word.size() == 2 && static_cast<unsigned char>(word[0]) < 0x80 && static_cast<unsigned char>(word[1]) < 0x80) {
        std::string out;
        out += asciiUpper(word[0]);
        out += '-';
        out += asciiUpper(word[1]);
        return out;
    }
    return std::string(word);
}

// ---- numbers.rs ---------------------------------------------------------------------------

std::optional<std::string> numbers(std::string_view word, Lang lang) {
    const auto [body, end] = splitSuffix(word);
    if (body.size() >= 2 && body[0] == '0' && allDigits(body)) return spell(body, lang) + std::string(end);
    if (auto r = range(body, lang)) return *r + std::string(end);
    // The Spanish ordinals written "1.er" and "3.er", which the URL rule would otherwise read
    // as a domain.
    if (lang == Lang::Es && endsWith(body, "er")) {
        std::string_view n = body.substr(0, body.size() - 2);
        if (endsWith(n, ".")) n.remove_suffix(1);
        if (n == "1") return "primer" + std::string(end);
        if (n == "3") return "tercer" + std::string(end);
    }
    if (auto nw = numberWords(word, lang)) return nw->first + std::string(nw->second);
    const size_t dash = word.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const std::string_view head = word.substr(0, dash), tail = word.substr(dash);
    size_t n;
    const char32_t afterDash = firstChar(tail.substr(1), &n);
    if (n == 0 || !isAlphabetic(afterDash)) return std::nullopt;
    auto hw = numberWords(head, lang);
    if (!hw || !hw->second.empty()) return std::nullopt;
    return hw->first + std::string(tail);
}

std::optional<std::pair<std::string, size_t>> digitRun(const std::vector<std::string_view>& words, Lang lang) {
    if (words.empty()) return std::nullopt;
    auto group = [](std::string_view s) {
        if (s.size() < 1 || s.size() > 8) return false;
        bool anyDigit = false;
        for (char c : s) {
            const bool d = c >= '0' && c <= '9';
            if (!d && !(c >= 'A' && c <= 'Z')) return false;
            anyDigit |= d;
        }
        return anyDigit;
    };
    const bool plus = startsWith(words[0], "+");
    std::vector<std::string_view> bodies;
    std::string_view end;
    for (std::string_view word : words) {
        auto [body, sfx] = splitSuffix(word);
        if (bodies.empty() && plus) body = body.substr(1);
        if (!group(body)) break;
        bodies.push_back(body);
        if (!sfx.empty()) {
            end = sfx;
            break;
        }
    }
    const bool years = std::all_of(bodies.begin(), bodies.end(), [](std::string_view b) {
        return b.size() == 4 && (b.substr(0, 2) == "19" || b.substr(0, 2) == "20");
    });
    const bool isLong = bodies.size() >= 3 && !years;
    const bool anyZero = std::any_of(bodies.begin(), bodies.end(), [](std::string_view b) { return startsWith(b, "0"); });
    if (!isLong && !(bodies.size() == 2 && (plus || anyZero))) return std::nullopt;
    std::vector<std::string> spelled;
    for (auto b : bodies) spelled.push_back(spell(b, lang));
    std::string out = join(spelled, ", ");
    if (plus) out = std::string(special(lang).plus) + " " + out;
    return std::make_pair(out + std::string(end), bodies.size());
}

std::optional<std::pair<std::string, size_t>> endingIn(std::optional<std::string_view> prev,
                                                       const std::vector<std::string_view>& words, Lang lang) {
    if (!prev) return std::nullopt;
    // Only ASCII can lowercase to "ending", "ends" or "ended".
    std::string lower(*prev);
    for (auto& c : lower) c = asciiLower(c);
    if (!(lower == "ending" || lower == "ends" || lower == "ended")) return std::nullopt;
    if (words.empty() || words[0] != "in" || words.size() < 2) return std::nullopt;
    const auto [body, end] = splitSuffix(words[1]);
    if (!startsWith(body, "-")) return std::nullopt;
    const std::string_view code = body.substr(1);
    bool anyDigit = false;
    for (char c : code) {
        const bool d = c >= '0' && c <= '9';
        if (!d && !(c >= 'A' && c <= 'Z') && c != '-') return std::nullopt;
        anyDigit |= d;
    }
    if (!anyDigit) return std::nullopt;
    return std::make_pair("in " + spell(code, lang) + std::string(end), size_t(2));
}

} // namespace detail

// ===========================================================================================
// currency.rs
// ===========================================================================================

namespace {

enum class Currency { Dollar, Euro, Pound, Yen, Cent, Real };

std::optional<Currency> fromSymbol(char32_t c) {
    switch (c) {
    case U'$': return Currency::Dollar;
    case U'€': return Currency::Euro;
    case U'£': return Currency::Pound;
    case U'¥': return Currency::Yen;
    case U'¢': return Currency::Cent;
    default: return std::nullopt;
    }
}

std::pair<const char*, const char*> currencyName(Currency cur, Lang lang) {
    switch (cur) {
    case Currency::Dollar:
        if (lang == Lang::En || lang == Lang::Fr) return {"dollar", "dollars"};
        if (lang == Lang::De) return {"Dollar", "Dollar"};
        return {"dólar", "dólares"};
    case Currency::Euro:
        if (lang == Lang::De) return {"Euro", "Euro"};
        return {"euro", "euros"};
    case Currency::Pound:
        if (lang == Lang::En) return {"pound", "pounds"};
        if (lang == Lang::Fr) return {"livre", "livres"};
        if (lang == Lang::De) return {"Pfund", "Pfund"};
        return {"libra", "libras"};
    case Currency::Yen:
        if (lang == Lang::De) return {"Yen", "Yen"};
        if (lang == Lang::En) return {"yen", "yen"};
        if (lang == Lang::Fr) return {"yen", "yens"};
        if (lang == Lang::Es) return {"yen", "yenes"};
        return {"iene", "ienes"};
    case Currency::Cent:
        if (lang == Lang::En || lang == Lang::Fr) return {"cent", "cents"};
        if (lang == Lang::De) return {"Cent", "Cent"};
        return {"centavo", "centavos"};
    case Currency::Real:
        return {"real", "reais"};
    }
    return {"", ""};
}

// `word` without one leading currency symbol, and the symbol's currency.
std::optional<std::pair<Currency, std::string_view>> stripSymbolPrefix(std::string_view word) {
    size_t n;
    const char32_t c = firstChar(word, &n);
    if (n == 0) return std::nullopt;
    if (auto cur = fromSymbol(c)) return std::make_pair(*cur, word.substr(n));
    return std::nullopt;
}

std::optional<std::pair<Currency, std::string_view>> splitSymbol(std::string_view word, Lang lang) {
    if (lang == Lang::Pt && startsWith(word, "R$")) return std::make_pair(Currency::Real, word.substr(2));
    if (auto p = stripSymbolPrefix(word)) return p;
    size_t n;
    const char32_t c = lastChar(word, &n);
    if (n == 0) return std::nullopt;
    if (auto cur = fromSymbol(c)) return std::make_pair(*cur, word.substr(0, word.size() - n));
    return std::nullopt;
}

std::optional<std::pair<std::string_view, const char*>> splitScale(std::string_view amount, Lang lang) {
    if (lang != Lang::En) return std::nullopt;
    static const std::pair<const char*, const char*> kSuffixes[] = {
        {"thousand", "thousand"}, {"million", "million"}, {"billion", "billion"},
        {"trillion", "trillion"}, {"bn", "billion"}, {"BN", "billion"}, {"mn", "million"},
        {"tn", "trillion"}, {"k", "thousand"}, {"K", "thousand"}, {"m", "million"},
        {"M", "million"}, {"b", "billion"}, {"B", "billion"},
    };
    for (const auto& [sfx, scale] : kSuffixes) {
        if (!endsWith(amount, sfx)) continue;
        const std::string_view number = amount.substr(0, amount.size() - std::strlen(sfx));
        if (!number.empty() && number.back() >= '0' && number.back() <= '9') return std::make_pair(number, scale);
    }
    return std::nullopt;
}

bool scaleWord(std::string_view word, Lang lang) {
    static const std::vector<std::vector<std::string_view>> kWords = {
        {"thousand", "million", "billion", "trillion"},
        {"mille", "million", "millions", "milliard", "milliards"},
        {"Tausend", "Million", "Millionen", "Milliarde", "Milliarden"},
        {"mil", "millón", "millones"},
        {"mil", "milhão", "milhões", "bilhão", "bilhões"},
    };
    const auto& words = kWords[int(lang)];
    return std::find(words.begin(), words.end(), word) != words.end();
}

// `amount` followed by the currency's `name`, with the linking "de" a scale noun takes in
// French, Spanish and Portuguese ("2 millions de dollars", "2 millions d'euros").
std::string withName(const std::string& amount, std::string_view name, Lang lang) {
    static const std::vector<std::vector<std::string_view>> kNouns = {
        {},
        {"million", "millions", "milliard", "milliards"},
        {},
        {"millón", "millones"},
        {"milhão", "milhões", "bilhão", "bilhões"},
    };
    const auto& nouns = kNouns[int(lang)];
    const bool afterNoun = std::any_of(nouns.begin(), nouns.end(), [&](std::string_view n) { return endsWith(amount, n); });
    if (!afterNoun) return amount + " " + std::string(name);
    size_t n;
    const char32_t first = firstChar(name, &n);
    if (lang == Lang::Fr && (first == U'a' || first == U'e' || first == U'i' || first == U'o' || first == U'u' || first == U'é'))
        return amount + " d'" + std::string(name);
    return amount + " de " + std::string(name);
}

std::optional<std::string> withScale(std::string_view amount, std::string_view scale, Currency cur, Lang lang) {
    auto nw = numberWords(amount, lang);
    if (!nw || !nw->second.empty()) return std::nullopt;
    return withName(nw->first + " " + std::string(scale), currencyName(cur, lang).second, lang);
}

std::optional<std::string> withCents(std::string_view amount, Currency cur, Lang lang) {
    std::string_view whole, cents;
    if (lang == Lang::En) {
        const size_t dot = amount.rfind('.');
        if (dot == std::string_view::npos) return std::nullopt;
        whole = amount.substr(0, dot);
        cents = amount.substr(dot + 1);
        bool ok = allDigits(whole);
        if (!ok) {
            ok = true;
            const auto groups = split(whole, ',');
            for (size_t i = 0; i < groups.size(); ++i) {
                if (!allDigits(groups[i]) || (i == 0 ? groups[i].size() > 3 : groups[i].size() != 3)) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) return std::nullopt;
    } else if (lang == Lang::Fr) {
        const size_t comma = amount.rfind(',');
        if (comma == std::string_view::npos) return std::nullopt;
        whole = amount.substr(0, comma);
        cents = amount.substr(comma + 1);
        if (!allDigits(whole)) return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (cents.size() != 2 || !allDigits(cents)) return std::nullopt;
    auto nw = numberWords(whole, lang);
    if (!nw) return std::nullopt;
    const std::string& wholeWords = nw->first;
    const auto [one, many] = currencyName(cur, lang);
    const std::string units = withName(wholeWords, wholeWords == "1" ? one : many, lang);
    if (cents == "00") return units;
    if (cents[0] == '0') cents.remove_prefix(1);
    const char* andWord = "and";
    const char* centWord = "cents";
    if (lang == Lang::Fr) {
        andWord = "et";
        centWord = cur == Currency::Euro ? "centimes" : "cents";
    }
    if (wholeWords == "0") return std::string(cents) + " " + centWord;
    return units + " " + andWord + " " + std::string(cents) + " " + centWord;
}

} // namespace

namespace detail {

std::optional<std::string> currency(std::string_view word, Lang lang) {
    const auto [body, sfx] = splitSuffix(word);
    // "-$500" reads as "$-500" does.
    if (startsWith(body, "-")) {
        const std::string_view b = body.substr(1);
        std::optional<std::string_view> amount;
        if (auto p = stripSymbolPrefix(b)) amount = p->second;
        else if (lang == Lang::Pt && startsWith(b, "R$")) amount = b.substr(2);
        if (amount) {
            const std::string_view symbol = b.substr(0, b.size() - amount->size());
            return currency(std::string(symbol) + "-" + std::string(*amount) + std::string(sfx), lang);
        }
    }
    // The symbol leads or trails, never both: "$5€" is not an amount.
    const auto split = splitSymbol(body, lang);
    if (!split) return std::nullopt;
    const auto [cur, amount] = *split;
    if (auto sc = splitScale(amount, lang)) {
        auto spoken = withScale(sc->first, sc->second, cur, lang);
        if (!spoken) return std::nullopt;
        return *spoken + std::string(sfx);
    }
    if (cur != Currency::Cent) {
        if (auto spoken = withCents(amount, cur, lang)) return *spoken + std::string(sfx);
    }
    auto nw = numberWords(amount, lang);
    if (!nw) return std::nullopt;
    // Always the plural, even for 1 ("$1" reads "1 dollars"), except for the cent sign.
    const auto [one, many] = currencyName(cur, lang);
    const char* name = (cur == Currency::Cent && amount == "1") ? one : many;
    return withName(nw->first, name, lang) + std::string(sfx);
}

std::optional<std::pair<std::string, size_t>> scaled(const std::vector<std::string_view>& words, Lang lang) {
    if (words.size() < 2) return std::nullopt;
    const auto [scaleRaw, sfx] = splitSuffix(words[1]);
    // "$2 million", or "2 € millions" once the character pass has joined it to "2€ millions".
    const auto split = splitSymbol(words[0], lang);
    if (!split) return std::nullopt;
    const auto [cur, amount] = *split;
    std::string_view scale = scaleRaw;
    if (lang == Lang::Pt && scale == "bi") scale = amount == "1" ? "bilhão" : "bilhões";
    else if (lang == Lang::Pt && scale == "mi") scale = amount == "1" ? "milhão" : "milhões";
    if (!scaleWord(scale, lang)) return std::nullopt;
    auto spoken = withScale(amount, scale, cur, lang);
    if (!spoken) return std::nullopt;
    return std::make_pair(*spoken + std::string(sfx), size_t(2));
}

// ---- dashed_digits.rs ---------------------------------------------------------------------

std::optional<std::string> dashedDigits(std::string_view word, Lang lang) {
    const auto [body, sfx] = splitSuffix(word);
    size_t separators = 0;
    for (char c : body) {
        const bool sep = c == '-' || c == '_';
        if (!(c >= '0' && c <= '9') && !sep) return std::nullopt;
        separators += sep;
    }
    if (separators < 2) return std::nullopt;
    // Groups, each with the separator after it; a leading, trailing or doubled separator fails.
    std::vector<std::pair<std::string_view, char>> groups;
    size_t start = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '-' && body[i] != '_') continue;
        if (i == start) return std::nullopt;
        groups.emplace_back(body.substr(start, i - start), body[i]);
        start = i + 1;
    }
    if (start == body.size()) return std::nullopt;
    groups.emplace_back(body.substr(start), '\0');

    auto dayMonth = [](std::string_view x, std::string_view y) {
        if (x.size() > 2 || y.size() > 2) return false;
        uint32_t a, b;
        if (!parseU32(x, &a) || !parseU32(y, &b)) return false;
        return (a >= 1 && a <= 31 && b >= 1 && b <= 12) || (a >= 1 && a <= 12 && b >= 1 && b <= 31);
    };
    auto looksLikeDate = [&]() {
        if (groups.size() != 3) return false;
        const auto a = groups[0].first, b = groups[1].first, c = groups[2].first;
        return (a.size() == 4 && dayMonth(b, c)) || ((c.size() == 4 || c.size() == 2) && dayMonth(a, b));
    };
    // Only a dashed sequence can be a date: the underscores of "2024_05_12" should be heard.
    if (body.find('_') == std::string_view::npos && looksLikeDate()) return std::nullopt;

    std::vector<std::string> words;
    for (const auto& [g, sep] : groups) {
        if (g.size() >= 4) {
            for (char c : g) words.emplace_back(1, c);
        } else {
            words.emplace_back(g);
        }
        if (sep == '_') words.emplace_back(special(lang).underscore);
    }
    return join(words, " ") + std::string(sfx);
}

// ---- emails.rs ----------------------------------------------------------------------------

std::optional<std::string> emails(std::string_view word, Lang lang) {
    const size_t at = word.find('@');
    if (at == std::string_view::npos) return std::nullopt;
    const std::string_view user = word.substr(0, at);
    std::string_view rest = word.substr(at + 1);
    if (user.empty()) return std::nullopt;
    for (char c : user) {
        if (!isAsciiAlnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.' && c != '+')
            return std::nullopt;
    }
    auto label = [](std::string_view s) -> std::optional<std::pair<std::string_view, std::string_view>> {
        size_t n = 0;
        while (n < s.size() && (isAsciiAlnum(static_cast<unsigned char>(s[n])) || s[n] == '-')) ++n;
        if (n == 0) return std::nullopt;
        return std::make_pair(s.substr(0, n), s.substr(n));
    };
    auto first = label(rest);
    if (!first) return std::nullopt;
    std::vector<std::string_view> labels{first->first};
    rest = first->second;
    while (startsWith(rest, ".")) {
        auto next = label(rest.substr(1));
        if (!next) break;
        labels.push_back(next->first);
        rest = next->second;
    }
    const auto sfx = suffix(rest);
    if (!sfx) return std::nullopt;
    if (labels.size() < 2) return std::nullopt;
    const std::string_view tld = labels.back();

    const SpecialChars& sc = special(lang);
    std::vector<std::string> words;
    // Push the words of `part`, with each separator `spoken` has a word for replaced by it.
    auto speak = [&](std::string_view part, auto spoken) {
        size_t start = 0;
        for (size_t i = 0; i < part.size(); ++i) {
            if (const char* w = spoken(part[i])) {
                if (start < i) words.emplace_back(part.substr(start, i - start));
                words.emplace_back(w);
                start = i + 1;
            }
        }
        if (start < part.size()) words.emplace_back(part.substr(start));
    };
    speak(user, [&](char c) -> const char* {
        switch (c) {
        case '-': return sc.dash;
        case '.': return sc.dot;
        case '+': return sc.plus;
        case '_': return sc.underscore;
        default: return nullptr;
        }
    });
    words.emplace_back(sc.at);
    for (size_t i = 0; i + 1 < labels.size(); ++i) {
        if (i > 0) words.emplace_back(sc.dot);
        speak(labels[i], [&](char c) -> const char* { return c == '-' ? sc.dash : nullptr; });
    }
    words.emplace_back(sc.dot);
    return join(words, " ") + " " + spellShort(tld) + std::string(*sfx);
}

// ---- tlds.rs ------------------------------------------------------------------------------

bool isTld(std::string_view label) {
    // label.to_uppercase(): only ASCII letters and the few code points whose uppercase is
    // ASCII ("ß" -> "SS", "ﬁ" -> "FI", ...) can produce an entry of the (ASCII) table.
    std::string upper;
    for (size_t i = 0; i < label.size();) {
        size_t n;
        const char32_t c = decodeAt(label, i, &n);
        i += n;
        if (c < 0x80) {
            upper += asciiUpper(char(c));
            continue;
        }
        const UpperToAscii* hit = nullptr;
        for (const auto& u : kUpperToAscii)
            if (u.cp == c) { hit = &u; break; }
        if (!hit) return false;
        upper += hit->upper;
    }
    const auto* begin = std::begin(kTlds);
    const auto* end = std::end(kTlds);
    const auto* it = std::lower_bound(begin, end, upper, [](const char* a, const std::string& b) {
        return std::string_view(a) < std::string_view(b);
    });
    return it != end && std::string_view(*it) == upper;
}

// ---- urls.rs ------------------------------------------------------------------------------

std::optional<std::string> urls(std::string_view word, Lang lang) {
    const SpecialChars& sc = special(lang);
    std::vector<std::string> words;
    std::string_view rest = word;
    if (startsWith(word, "https://")) {
        words.emplace_back("H-T-T-P-S");
        rest = word.substr(8);
    } else if (startsWith(word, "http://")) {
        words.emplace_back("H-T-T-P");
        rest = word.substr(7);
    }
    if (!words.empty()) {
        words.emplace_back(sc.colon);
        words.emplace_back(sc.slash);
        words.emplace_back(sc.slash);
    }
    // The run of characters `s` starts with that `keep` accepts, and the rest.
    auto run = [](std::string_view s, bool withSlash) -> std::optional<std::pair<std::string_view, std::string_view>> {
        size_t i = 0;
        while (i < s.size()) {
            size_t n;
            const char32_t c = decodeAt(s, i, &n);
            if (!(isAlphanumeric(c) || c == U'-' || (withSlash && c == U'/'))) break;
            i += n;
        }
        if (i == 0) return std::nullopt;
        return std::make_pair(s.substr(0, i), s.substr(i));
    };
    auto first = run(rest, false);
    if (!first) return std::nullopt;
    std::vector<std::string_view> labels{first->first};
    rest = first->second;
    while (startsWith(rest, ".")) {
        auto next = run(rest.substr(1), false);
        if (!next) break;
        labels.push_back(next->first);
        rest = next->second;
    }
    std::optional<std::string_view> path;
    if (auto p = run(rest, true)) {
        path = p->first;
        rest = p->second;
    }
    const auto sfx = suffix(rest);
    if (!sfx) return std::nullopt;
    const std::string_view tld = labels.back();
    if (labels.size() < 2 || !isTld(tld)) return std::nullopt;
    bool anyUpper = false, anyLower = false;
    for (size_t i = 0; i < tld.size();) {
        size_t n;
        const char32_t c = decodeAt(tld, i, &n);
        anyUpper |= isUppercase(c);
        anyLower |= isLowercase(c);
        i += n;
    }
    std::string first3(labels[0]);
    for (auto& c : first3) c = asciiLower(c);
    if (anyUpper && anyLower && words.size() == 0 && first3 != "www") return std::nullopt;

    // Dashes and slashes spoken, each ASCII digit a word of its own, two-letter words spelled.
    auto speak = [&](std::string_view part) {
        size_t start = 0;
        for (size_t i = 0; i < part.size(); ++i) {
            const char c = part[i];
            std::string w;
            if (c == '-') w = sc.dash;
            else if (c == '/') w = sc.slash;
            else if (c >= '0' && c <= '9') w = std::string(1, c);
            else continue;
            if (start < i) words.push_back(spellShort(part.substr(start, i - start)));
            words.push_back(w);
            start = i + 1;
        }
        if (start < part.size()) words.push_back(spellShort(part.substr(start)));
    };
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) words.emplace_back(sc.dot);
        speak(labels[i]);
    }
    if (path) speak(*path);
    return join(words, " ") + std::string(*sfx);
}

// ---- abbreviations.rs ---------------------------------------------------------------------

std::optional<std::string> abbreviations(std::string_view word, Lang lang) {
    if (lang != Lang::En) return std::nullopt;
    static const std::pair<std::string_view, std::string_view> kEn[] = {
        {"Lt", "Lieutenant"}, {"Lt.", "Lieutenant"}, {"Col.", "Colonel"}, {"Sgt.", "Sergeant"},
        {"Sgt", "Sergeant"}, {"Capt.", "Captain"}, {"Cpl.", "Corporal"}, {"Gen.", "General"},
        {"Adm.", "Admiral"}, {"Sen.", "Senator"}, {"Sens.", "Senators"}, {"Rep.", "Representative"},
        {"Reps.", "Representatives"}, {"Gov.", "Governor"}, {"Pres.", "President"},
        {"Det.", "Detective"}, {"Insp.", "Inspector"}, {"Supt.", "Superintendent"}, {"Ft.", "Fort"},
        {"Mt.", "Mount"}, {"Jan.", "January"}, {"Feb.", "February"}, {"Mar.", "March"},
        {"Apr.", "April"}, {"Jun.", "June"}, {"Jul.", "July"}, {"Aug.", "August"},
        {"Sep.", "September"}, {"Sept.", "September"}, {"Oct.", "October"}, {"Nov.", "November"},
        {"Dec.", "December"}, {"Mon.", "Monday"}, {"Tue.", "Tuesday"}, {"Tues.", "Tuesday"},
        {"Wed.", "Wednesday"}, {"Thu.", "Thursday"}, {"Thur.", "Thursday"}, {"Thurs.", "Thursday"},
        {"Fri.", "Friday"}, {"Sat.", "Saturday"}, {"Sun.", "Sunday"}, {"vol.", "volume"},
        {"Vol.", "Volume"}, {"ed.", "edition"}, {"approx.", "approximately"},
        {"Approx.", "Approximately"}, {"dept.", "department"}, {"Dept.", "Department"},
        {"vs.", "versus"}, {"w/", "with"}, {"W/", "With"}, {"w/o", "without"}, {"W/o", "Without"},
    };
    const auto [body, sfx0] = splitSuffix(word);
    std::string_view abbreviation = body, sfx = sfx0;
    if (startsWith(sfx0, ".")) {
        abbreviation = word.substr(0, body.size() + 1);
        sfx = sfx0.substr(1);
    }
    for (const auto& [shortForm, expansion] : kEn)
        if (shortForm == abbreviation) return std::string(expansion) + std::string(sfx);
    return std::nullopt;
}

std::optional<std::pair<std::string, size_t>> ranks(const std::vector<std::string_view>& words, Lang lang) {
    if (words.size() < 2) return std::nullopt;
    const auto [rank, sfx] = splitSuffix(words[1]);
    if (lang == Lang::En && words[0] == "Lt" && rank == "Col")
        return std::make_pair("Lieutenant Colonel" + std::string(sfx), size_t(2));
    return std::nullopt;
}

// ---- elongations.rs -----------------------------------------------------------------------

std::optional<std::string> elongations(std::string_view word, Lang lang) {
    if (lang != Lang::En) return std::nullopt;
    const auto [body, sfx] = splitSuffix(word);
    const std::u32string chars = decode(body);
    auto stretched = [](char32_t c) {
        const char l = asciiLower(char(c));
        return l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u' || l == 'y' || l == 'h' || l == 'w';
    };
    bool allAlpha = true, allUpper = true, anyRun = false;
    for (char32_t c : chars) {
        allAlpha &= isAsciiAlpha(c);
        allUpper &= isAsciiUpper(c);
    }
    for (size_t i = 0; i + 3 <= chars.size(); ++i)
        if (chars[i] == chars[i + 1] && chars[i + 1] == chars[i + 2] && stretched(chars[i])) anyRun = true;
    if (!allAlpha || allUpper || !anyRun) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < chars.size(); ++i) {
        const char32_t c = chars[i];
        const bool run = i >= 1 && chars[i - 1] == c && stretched(c);
        const size_t lo = i >= 2 ? i - 2 : 0;
        const size_t hi = std::min(i + 3, chars.size());
        bool isLong = false;
        for (size_t j = lo; j + 3 <= hi; ++j)
            if (chars[j] == c && chars[j + 1] == c && chars[j + 2] == c) isLong = true;
        if (!(run && isLong)) out += char(c);
    }
    return out + std::string(sfx);
}

// ---- phones.rs ----------------------------------------------------------------------------

namespace {

// The digits of `number` in groups of the lengths in `lens`, then of `then` digits until the
// end; with `dashed`, a group's digits are joined by dashes, and an English zero is "zero".
std::string phoneGroup(std::string_view number, const std::vector<size_t>& lens, size_t then, bool dashed, Lang lang) {
    std::vector<std::string> groups, group;
    for (char d : number) {
        if (dashed && lang == Lang::En && d == '0') group.emplace_back("zero");
        else group.emplace_back(1, d);
        const size_t target = groups.size() < lens.size() ? lens[groups.size()] : then;
        if (group.size() >= target) {
            groups.push_back(join(group, dashed ? "-" : ""));
            group.clear();
        }
    }
    if (!group.empty()) groups.push_back(join(group, dashed ? "-" : ""));
    return join(groups, " ");
}

std::string phoneUk(std::string_view number, Lang lang) {
    const size_t zero = startsWith(number, "0") ? 1 : 0;
    if (startsWith(number.substr(zero), "2")) return phoneGroup(number, {2 + zero, 4, 4}, 4, true, lang);
    return phoneGroup(number, {3 + zero, 3, 4}, 4, true, lang);
}

std::string phoneDe(std::string_view number, Lang lang) {
    const size_t zero = startsWith(number, "0") ? 1 : 0;
    const std::string_view national = number.substr(zero);
    if (startsWith(national, "15") || startsWith(national, "16") || startsWith(national, "17"))
        return phoneGroup(number, {2 + zero, 1, 1}, 2, true, lang);
    if (startsWith(national, "30") || startsWith(national, "40") || startsWith(national, "69"))
        return phoneGroup(number, {2 + zero}, 2, true, lang);
    return phoneGroup(number, {3 + zero}, 2, true, lang);
}

std::optional<std::pair<std::string_view, std::string_view>> countryCode(std::string_view number) {
    static const std::vector<std::string_view> kCodes1 = {"1", "7"};
    static const std::vector<std::string_view> kCodes2 = {
        "20", "27", "30", "31", "32", "33", "34", "36", "39", "40", "41", "43", "44", "45", "46", "47",
        "48", "49", "51", "52", "53", "54", "55", "56", "57", "58", "60", "61", "62", "63", "64", "65",
        "66", "81", "82", "84", "86", "90", "91", "92", "93", "94", "95", "98",
    };
    static const std::vector<std::string_view> kCodes3 = {
        "211", "212", "213", "216", "218", "220", "221", "222", "223", "224", "225", "226", "227",
        "228", "229", "230", "231", "232", "233", "234", "235", "236", "237", "238", "239", "240",
        "241", "242", "243", "244", "245", "248", "249", "250", "251", "252", "253", "254", "255",
        "256", "257", "258", "260", "261", "262", "263", "264", "265", "267", "268", "269", "291",
        "297", "298", "350", "351", "352", "353", "354", "355", "356", "357", "358", "359", "370",
        "371", "372", "374", "375", "376", "377", "378", "379", "380", "381", "382", "383", "385",
        "386", "387", "389", "420", "421", "423", "500", "501", "502", "503", "504", "505", "506",
        "507", "509", "591", "592", "593", "594", "595", "597", "598", "599", "670", "673", "674",
        "675", "676", "677", "678", "679", "680", "682", "683", "685", "686", "687", "688", "689",
        "691", "692", "850", "852", "853", "855", "856", "880", "886", "960", "961", "962", "963",
        "964", "965", "966", "967", "968", "970", "971", "972", "973", "974", "975", "976", "977",
        "992", "993", "994", "995", "996", "998",
    };
    const std::vector<std::string_view>* codes[3] = {&kCodes1, &kCodes2, &kCodes3};
    for (size_t i = 0; i < 3; ++i) {
        if (number.size() < i + 1) continue;
        const std::string_view code = number.substr(0, i + 1);
        if (std::find(codes[i]->begin(), codes[i]->end(), code) != codes[i]->end())
            return std::make_pair(code, number.substr(i + 1));
    }
    return std::nullopt;
}

// times.rs: one or two digits of hours, a separator, two to four digits of minutes, the rest.
struct TimeParts { std::string_view hour, minutes; char separator; std::string_view rest; };

std::optional<TimeParts> parseTime(std::string_view word, std::string_view separators) {
    auto h = detail::digits(word, 1, 2);
    if (!h) return std::nullopt;
    auto [hour, rest] = *h;
    if (rest.empty() || separators.find(rest[0]) == std::string_view::npos) return std::nullopt;
    const char separator = rest[0];
    auto m = detail::digits(rest.substr(1), 2, 4);
    if (!m) return std::nullopt;
    if (hour.size() == 2 && hour[0] == '0') hour.remove_prefix(1);
    return TimeParts{hour, m->first, separator, m->second};
}

} // namespace

std::optional<std::string> phones(std::string_view word, Lang lang) {
    const bool international = startsWith(word, "+");
    const std::string_view rest0 = international ? word.substr(1) : word;
    auto d = digits(rest0, 1, std::string_view::npos);
    if (!d) return std::nullopt;
    const auto [number, rest] = *d;
    const auto sfx = suffix(rest);
    if (!sfx) return std::nullopt;
    std::string spoken;
    if (international) {
        if (number.size() < 6) return std::nullopt;
        auto cc = countryCode(number);
        if (!cc) return std::nullopt;
        const auto [code, local] = *cc;
        std::string grouped;
        if (code == "1") grouped = phoneGroup(local, {3, 3, 4}, 3, true, lang);
        else if (code == "34" || code == "351") grouped = phoneGroup(local, {3, 3, 3}, 3, true, lang);
        else if (code == "44") grouped = phoneUk(local, lang);
        else if (code == "49") grouped = phoneDe(local, lang);
        else grouped = phoneGroup(local, {2 - local.size() % 2}, 2, lang != Lang::Fr, lang);
        spoken = std::string(special(lang).plus) + " " + std::string(code) + " " + grouped;
    } else {
        if (!startsWith(number, "0")) return std::nullopt;
        if (lang == Lang::Fr && number.size() == 10) spoken = phoneGroup(number, {}, 2, false, lang);
        else if (lang == Lang::En && number.size() == 11) spoken = phoneUk(number, lang);
        else if (lang == Lang::De && (number.size() == 10 || number.size() == 11)) spoken = phoneDe(number, lang);
        else return std::nullopt;
    }
    return spoken + std::string(*sfx);
}

// ---- times.rs -----------------------------------------------------------------------------

std::optional<std::string> times(std::string_view word, Lang lang) {
    if (lang == Lang::En) {
        auto t = parseTime(word, ":.");
        if (!t) return std::nullopt;
        std::string_view meridiem, rest = t->rest;
        for (std::string_view m : {"AM", "PM", "am", "pm"}) {
            if (startsWith(rest, m)) {
                meridiem = m;
                rest = rest.substr(m.size());
                break;
            }
        }
        const auto sfx = suffix(rest);
        if (!sfx) return std::nullopt;
        if (t->separator == '.' && meridiem.empty()) return std::nullopt;
        const std::string time = t->minutes == "00" ? std::string(t->hour)
                                                    : std::string(t->hour) + "-" + std::string(t->minutes);
        if (meridiem.empty()) return time + std::string(*sfx);
        std::string upper(meridiem);
        for (auto& c : upper) c = asciiUpper(c);
        return time + " " + upper + std::string(*sfx);
    }
    if (lang == Lang::Fr) {
        auto t = parseTime(word, ":hH");
        if (!t) return std::nullopt;
        const auto sfx = suffix(t->rest);
        if (!sfx) return std::nullopt;
        return std::string(t->hour) + "h" + std::string(t->minutes) + std::string(*sfx);
    }
    if (lang == Lang::De) {
        auto t = parseTime(word, ":.");
        if (!t) return std::nullopt;
        const auto sfx = suffix(t->rest);
        if (!sfx) return std::nullopt;
        if (t->minutes == "00") return std::string(t->hour) + " Uhr" + std::string(*sfx);
        return std::string(t->hour) + " Uhr " + std::string(t->minutes) + std::string(*sfx);
    }
    return std::nullopt;
}

// ---- dates.rs -----------------------------------------------------------------------------

std::optional<std::string> dates(std::string_view word, Lang) {
    auto first = digits(word, 1, 2);
    if (!first || !startsWith(first->second, "/")) return std::nullopt;
    auto second = digits(first->second.substr(1), 1, 2);
    if (!second) return std::nullopt;
    std::optional<std::string_view> year;
    std::string_view rest = second->second;
    if (startsWith(rest, "/")) {
        if (auto y = digits(rest.substr(1), 2, 4)) {
            year = y->first;
            rest = y->second;
        }
    }
    const auto sfx = suffix(rest);
    if (!sfx) return std::nullopt;
    std::string out = std::string(first->first) + "-" + std::string(second->first);
    if (year) out += " " + std::string(*year);
    return out + std::string(*sfx);
}

// ---- rewrite.rs ---------------------------------------------------------------------------

namespace {

using Rewrite = std::optional<std::string> (*)(std::string_view, Lang);

struct Rule {
    const char* name;
    Rewrite rewrite;
    bool isDefault;
};

// Every rule, in the order they are tried. The first rule that claims a word wins.
const Rule kRules[] = {
    {"numbers", &detail::numbers, true},
    {"currency", &detail::currency, true},
    {"dashed-digits", &detail::dashedDigits, true},
    {"emails", &detail::emails, true},
    {"urls", &detail::urls, true},
    {"abbreviations", &detail::abbreviations, true},
    {"elongations", &detail::elongations, true},
    {"phones", &detail::phones, false},
    {"times", &detail::times, false},
    {"dates", &detail::dates, false},
};
constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

} // namespace

std::optional<std::string> rewriteWord(std::string_view word, Lang lang, Rules rules) {
    for (size_t i = 0; i < kRuleCount; ++i) {
        if (!(rules.bits & (1u << i))) continue;
        if (auto out = kRules[i].rewrite(word, lang)) return out;
    }
    return std::nullopt;
}

} // namespace detail

// ===========================================================================================
// Rules / Lang
// ===========================================================================================

Rules Rules::none() { return Rules{0}; }

Rules Rules::defaults() {
    Rules r;
    for (size_t i = 0; i < detail::kRuleCount; ++i)
        if (detail::kRules[i].isDefault) r.bits |= 1u << i;
    return r;
}

Rules Rules::all() { return Rules{(1u << detail::kRuleCount) - 1}; }

bool Rules::has(std::string_view name) const {
    for (size_t i = 0; i < detail::kRuleCount; ++i)
        if (name == detail::kRules[i].name) return (bits & (1u << i)) != 0;
    return false;
}

namespace {

std::string_view trim(std::string_view s) {
    // Rust's str::trim: Unicode whitespace at both ends.
    while (!s.empty()) {
        size_t n;
        const char32_t c = firstChar(s, &n);
        if (!isWhitespace(c)) break;
        s.remove_prefix(n);
    }
    while (!s.empty()) {
        size_t n;
        const char32_t c = lastChar(s, &n);
        if (!isWhitespace(c)) break;
        s.remove_suffix(n);
    }
    return s;
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = asciiLower(c);
    return out;
}

} // namespace

std::optional<Rules> Rules::parse(std::string_view s) {
    const std::string v = lowerAscii(trim(s));
    if (v == "default") return defaults();
    if (v == "all") return all();
    if (v == "none" || v == "off") return none();
    Rules set = none();
    for (std::string_view name : split(v, ',')) {
        name = trim(name);
        size_t i = 0;
        while (i < detail::kRuleCount && name != detail::kRules[i].name) ++i;
        if (i == detail::kRuleCount) return std::nullopt;
        set.bits |= 1u << i;
    }
    return set;
}

std::optional<Lang> parseLang(std::string_view s) {
    const std::string v = lowerAscii(trim(s));
    if (v == "en") return Lang::En;
    if (v == "fr") return Lang::Fr;
    if (v == "de") return Lang::De;
    if (v == "es") return Lang::Es;
    if (v == "pt") return Lang::Pt;
    return std::nullopt;
}

const char* langCode(Lang lang) {
    switch (lang) {
    case Lang::En: return "en";
    case Lang::Fr: return "fr";
    case Lang::De: return "de";
    case Lang::Es: return "es";
    case Lang::Pt: return "pt";
    }
    return "";
}

// ===========================================================================================
// The character pass and the word pass (preprocess.rs)
// ===========================================================================================

namespace {

// Units written with a slash between letters ("mg/kg", "kJ/mol"), which keep it.
bool isSlashUnit(std::string_view p) {
    static const std::string_view kUnits[] = {
        "km", "kg", "mg", "ml", "mL", "dL", "mol", "kJ", "kW", "kWh", "Hz", "min", "hr", "hrs", "sec",
        "ft", "lb", "lbs", "oz", "mi", "mph", "kph", "yd", "cm", "mm", "yr", "wk", "mo", "kcal", "rpm",
    };
    return std::find(std::begin(kUnits), std::end(kUnits), p) != std::end(kUnits);
}

// `word` with each slash replaced by a space, when every part it separates is a word of two
// letters or more and not a unit.
std::optional<std::string> splitSlash(std::string_view word) {
    std::string_view body = word;
    while (!body.empty() && body.back() != '/' && isAsciiPunct(static_cast<unsigned char>(body.back())))
        body.remove_suffix(1);
    const auto parts = split(body, '/');
    if (parts.size() < 2) return std::nullopt;
    for (auto p : parts) {
        if (charCount(p) < 2 || isSlashUnit(p)) return std::nullopt;
        for (size_t i = 0; i < p.size();) {
            size_t n;
            if (!isAlphabetic(decodeAt(p, i, &n))) return std::nullopt;
            i += n;
        }
    }
    std::vector<std::string> owned(parts.begin(), parts.end());
    return join(owned, " ") + std::string(word.substr(body.size()));
}

std::string rewriteEach(const std::string& text, Lang lang, Rules rules) {
    std::vector<std::string> out;
    for (auto w : split(text, ' ')) {
        auto r = detail::rewriteWord(w, lang, rules);
        out.push_back(r ? *r : std::string(w));
    }
    return join(out, " ");
}

// Spell out the `@` and `+` of a word no rule claimed, and give the pieces they leave their own
// turn at the rules. A slash between words ("and/or") is read as a pause.
std::string spellSymbols(std::string_view word, Lang lang, Rules rules) {
    if (auto text = splitSlash(word)) return rewriteEach(*text, lang, rules);
    if (word.find('@') == std::string_view::npos && word.find('+') == std::string_view::npos)
        return std::string(word);
    Appender res;
    const SpecialChars& sc = special(lang);
    for (char32_t c : decode(word)) {
        if (c == U'@') res.pushSpoken(sc.at);
        else if (c == U'+') res.pushSpoken(sc.plus);
        else res.push(c);
    }
    return rewriteEach(res.intoString(), lang, rules);
}

std::string rewriteText(const std::string& text, Lang lang, Rules rules) {
    const auto words = split(text, ' ');
    std::vector<std::string> out;
    out.reserve(words.size());
    size_t i = 0;
    while (i < words.size()) {
        const std::vector<std::string_view> rest(words.begin() + long(i), words.end());
        std::optional<std::string_view> prev;
        if (!out.empty()) prev = out.back();
        std::optional<std::pair<std::string, size_t>> span;
        if (rules.has("currency")) span = detail::scaled(rest, lang);
        if (!span && rules.has("numbers")) span = detail::digitRun(rest, lang);
        if (!span && rules.has("numbers")) span = detail::endingIn(prev, rest, lang);
        if (!span && rules.has("abbreviations")) span = detail::ranks(rest, lang);
        if (span) {
            out.push_back(span->first);
            i += span->second;
            continue;
        }
        auto r = detail::rewriteWord(words[i], lang, rules);
        out.push_back(r ? *r : spellSymbols(words[i], lang, rules));
        i += 1;
    }
    return join(out, " ");
}

std::string_view stripClosingQuote(std::string_view text) {
    while (!text.empty() && text.back() == '"') text.remove_suffix(1);
    if (!text.empty() && text.back() == '\'') {
        const std::string_view rest = text.substr(0, text.size() - 1);
        if (!rest.empty()) {
            const char c = rest.back();
            if (c == '.' || c == '!' || c == '?' || c == ',' || c == ';') return rest;
        }
    }
    return text;
}

} // namespace

// tts_model.rs: prepare_text_prompt.
Prompt prepareTextPrompt(std::string_view raw) {
    Prompt p;
    const std::string_view t = trim(raw);
    if (t.empty()) return p; // ("", 3)
    // `text.split_whitespace()` (after turning \n and \r into spaces, which it splits on anyway).
    std::vector<std::u32string> words;
    std::u32string cur;
    for (char32_t c : decode(t)) {
        if (isWhitespace(c)) {
            if (!cur.empty()) words.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    p.framesAfterEos = words.size() <= 4 ? 3 : 1;

    std::u32string text;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) text.push_back(U' ');
        text += words[i];
    }
    // First character through char::to_uppercase, which may expand ("ß" -> "SS").
    if (!text.empty()) {
        const char32_t first = text[0];
        const auto* end = std::end(kToUpper);
        const auto* it = std::lower_bound(std::begin(kToUpper), end, first,
                                          [](const UpperMapping& m, char32_t c) { return m.cp < c; });
        if (it != end && it->cp == first) {
            std::u32string up;
            for (char32_t u : it->up)
                if (u) up.push_back(u);
            text = up + text.substr(1);
        }
    }
    if (!text.empty() && isAlphanumeric(text.back())) text.push_back(U'.');
    p.text = encode(text);
    return p;
}

std::string normalizeText(std::string_view input, Lang lang, Rules rules) {
    const std::u32string in = decode(input);
    Appender res;
    size_t pos = 0;
    std::optional<char32_t> prev;
    // Where the digits of a French number joined across a thousands space end, so that a
    // further group joins too: "1 000 000".
    size_t grouped = 0;
    auto peek = [&](size_t k = 0) -> std::optional<char32_t> {
        if (pos + k < in.size()) return in[pos + k];
        return std::nullopt;
    };
    auto peekIs = [&](auto pred) {
        auto p = peek();
        return p && pred(*p);
    };
    const SpecialChars& sc = special(lang);

    while (pos < in.size()) {
        const char32_t c = in[pos++];
        if (isDoubleQuote(c)) {
            res.push(U'"');
        } else if (isSingleQuote(c)) {
            res.push(U'\'');
        } else if (c == U'‐' || c == U'‑' || c == U'‒' || c == U'―') {
            res.push(U'-');
        } else if (c == U'★' || c == U'☆') {
            size_t full = c == U'★', empty = c == U'☆';
            while (auto next = peek()) {
                if (*next != U'★' && *next != U'☆') break;
                full += *next == U'★';
                empty += *next == U'☆';
                ++pos;
            }
            const bool half = peek() == std::optional<char32_t>(U'½');
            if (half) ++pos;
            std::string n = std::to_string(full);
            if (half) n += lang == Lang::En ? ".5" : ",5";
            std::optional<size_t> of;
            if (empty > 0) of = full + empty + (half ? 1 : 0);
            res.pushSpoken(stars(lang, n, of));
        } else if ((c == U'°' || c == U'º') && res.endsWithN()) {
            const std::string word = numberSign(lang);
            if (!res.b.empty() && res.b.back() == U'.') res.b.pop_back();
            const char32_t popped = res.b.back();
            res.b.pop_back();
            if (popped == U'N') {
                std::string cap = word;
                cap[0] = asciiUpper(cap[0]);
                res.pushSpoken(cap);
            } else {
                res.pushSpoken(word);
            }
        } else if (c == U'°') {
            res.pushSpoken(degrees(lang, res.endsWithOne()));
            const auto next = peek();
            const char* scale = nullptr;
            if (next == std::optional<char32_t>(U'C')) scale = "Celsius";
            else if (next == std::optional<char32_t>(U'F')) scale = "Fahrenheit";
            const auto after = peek(1);
            if (scale && !(after && isAlphanumeric(*after))) {
                ++pos;
                res.pushStr(scale);
            }
        } else if (auto f = fold(c)) {
            res.push(*f);
        } else if (auto s = subscript(c)) {
            res.push(*s);
        } else if (auto sup = superscript(c)) {
            std::string n;
            encodeTo(n, *sup);
            while (auto next = peek()) {
                auto d = superscript(*next);
                if (!d) break;
                encodeTo(n, *d);
                ++pos;
            }
            if (prev && (isAlphanumeric(*prev) || *prev == U')')) res.pushSpoken(power(lang, n));
            else res.pushStr(n);
        } else if (auto g = greek(lang, c)) {
            res.pushSpoken(*g);
        } else if (const char* sym = symbolWord(lang, c)) {
            res.pushSpoken(sym);
        } else if (c == U'±') {
            res.pushSpoken(plusMinus(lang));
        } else if (c == U'π') {
            res.pushSpoken(piWord(lang));
        } else if (c == U'❤' || c == U'♥') {
            if (peek() == std::optional<char32_t>(0xFE0F)) ++pos;
            res.pushSpoken(love(lang));
        } else if (c == U'~' && peekIs(isAsciiDigit)) {
            res.pushSpoken(about(lang));
        } else if (c == U'*' && (!prev || isWhitespace(*prev)) && peekIs(isAsciiDigit)) {
            res.pushSpoken(star(lang));
        } else if (c == U'–' || c == U'*' || c == U'—' || c == U'[' || c == U']' || c == U'{' || c == U'}') {
            res.pushWhitespace();
        } else if (c == U'•' || c == U'‣' || c == U'◦' || c == U'·' || c == U'→' || c == U'←' ||
                   c == U'↑' || c == U'↓' || c == U'➡' || c == U'➜') {
            res.pushWhitespace();
        } else if (isCurrencySymbol(c) && res.endsWithSpacedDigit() && !peekIs(isAsciiDigit)) {
            // "250 €": the symbol after the number and a space joins the number, so the
            // currency rule reads it. Not before a digit, where it leads the next amount.
            res.popWhitespace();
            res.push(c);
        } else if (isWhitespace(c) && lang == Lang::Fr &&
                   (res.b.size() == grouped ||
                    [&] {
                        auto lead = res.thousandsLead();
                        return lead && (*lead < 3 || thousandsGroups(in, pos) == 1);
                    }()) &&
                   thousandsGroups(in, pos) > 0) {
            // "40 000": French groups thousands with a space, which goes so that the numbers
            // rule reads one number rather than two.
            grouped = res.b.size() + 3;
        } else if (isWhitespace(c) && lang == Lang::Pt && res.b.size() >= 2 &&
                   res.b[res.b.size() - 2] == U'R' && res.b.back() == U'$' && peekIs(isAsciiDigit) &&
                   [&] {
                       for (size_t k = pos; k < in.size(); ++k)
                           if (!isAsciiDigit(in[k])) return in[k] != U'-';
                       return true;
                   }()) {
            // "R$ 250": the space after the real sign goes, unless a range follows.
        } else if (c == U'…') {
            res.push(U'.');
        } else if (c == U'=') {
            res.pushSpoken(sc.equals);
        } else if (c == U':' && prev && !isWhitespace(*prev) && peek() && !isWhitespace(*peek())) {
            res.push(U':');
        } else if (c == U';' || c == U':' || c == U'(' || c == U')') {
            res.popWhitespace();
            res.push(U',');
            res.pushWhitespace();
        } else if (isEmoji(c) || isControl(c) || isWhitespace(c)) {
            res.pushWhitespace();
        } else {
            res.push(c);
        }
        prev = c;
    }
    const std::string text = rewriteText(res.intoString(), lang, rules);
    return std::string(stripClosingQuote(text));
}

namespace detail {

std::string_view strTrim(std::string_view s) { return trim(s); }

std::vector<std::string_view> splitWhitespace(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0, i = 0;
    bool inWord = false;
    while (i < s.size()) {
        size_t n;
        const char32_t c = decodeAt(s, i, &n);
        if (isWhitespace(c)) {
            if (inWord) out.push_back(s.substr(start, i - start));
            inWord = false;
        } else if (!inWord) {
            start = i;
            inWord = true;
        }
        i += n;
    }
    if (inWord) out.push_back(s.substr(start));
    return out;
}

} // namespace detail

} // namespace gradium::tts::text
