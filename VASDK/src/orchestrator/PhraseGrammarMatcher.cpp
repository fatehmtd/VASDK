#include <VASDK/orchestrator/PhraseGrammarMatcher.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace va {
namespace orchestrator {

namespace {

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool isBuiltinParameterType(const std::string& lowerType) {
    static const std::unordered_set<std::string> kBuiltins = {
        "string", "phone", "email", "date", "time", "datetime",
        "number", "integer", "boolean", "enum", "object", "composite"
    };
    return kBuiltins.count(lowerType) != 0;
}

std::vector<std::string> tokenizeWords(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream iss(text);
    std::string w;
    while (iss >> w) words.push_back(toLower(w));
    return words;
}

//  Phrase grammar AST (spec §22.1, Draft v2.9)

struct PhraseAtom {
    enum class Kind { Literal, Wildcard, DotWildcard, Capture, BarePlaceholder, Group };
    Kind kind = Kind::Literal;
    bool optional = false;

    std::string literal;                                // Literal
    std::string captureName;                             // Capture
    std::string captureType;                             // Capture (lowercased)
    std::string canonical;                                // BarePlaceholder
    std::vector<std::vector<PhraseAtom>> alternatives;    // Group - one entry per '|' branch
};

struct CompiledPhrase {
    bool negative = false;
    std::vector<PhraseAtom> atoms;
    std::string sourceText;   // for error messages
};

//  Phrase string -> AST

class PhraseParser {
public:
    explicit PhraseParser(const std::string& text) : _text(text) {}

    CompiledPhrase parse() {
        CompiledPhrase phrase;
        phrase.sourceText = _text;
        if (peek() == '!') {
            phrase.negative = true;
            advance();
        }
        phrase.atoms = parseSequence(/*insideGroup=*/false);
        skipSpaces();
        if (!atEnd()) {
            fail(std::string("unexpected '") + peek() + "' outside any group");
        }
        return phrase;
    }

private:
    const std::string& _text;
    size_t _pos = 0;

    bool atEnd() const { return _pos >= _text.size(); }
    char peek() const { return atEnd() ? '\0' : _text[_pos]; }
    void advance() { ++_pos; }

    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error("[PhraseGrammarMatcher] malformed phrase \"" + _text + "\": " + reason);
    }

    void skipSpaces() {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(peek()))) advance();
    }

    std::vector<PhraseAtom> parseSequence(bool insideGroup) {
        std::vector<PhraseAtom> atoms;
        while (true) {
            skipSpaces();
            if (atEnd()) break;
            char c = peek();
            if (insideGroup && (c == ')' || c == '|')) break;
            atoms.push_back(parseAtom());
        }
        return atoms;
    }

    PhraseAtom parseAtom() {
        char c = peek();
        PhraseAtom atom;
        if (c == '(') {
            atom = parseGroup();
        } else if (c == '{') {
            atom = parseBraceToken();
        } else if (c == '*') {
            advance();
            atom.kind = PhraseAtom::Kind::Wildcard;
        } else if (c == '.' && isStandaloneDot()) {
            advance();
            atom.kind = PhraseAtom::Kind::DotWildcard;
        } else {
            atom.kind = PhraseAtom::Kind::Literal;
            atom.literal = toLower(readWord());
        }
        if (peek() == '?') {
            advance();
            atom.optional = true;
        }
        return atom;
    }

    // A bare '.' is the wildcard only when it stands alone as its own token -
    // not, e.g., inside a word like "e.g.".
    bool isStandaloneDot() const {
        size_t next = _pos + 1;
        if (next >= _text.size()) return true;
        char c = _text[next];
        return std::isspace(static_cast<unsigned char>(c)) || c == ')' || c == '(' ||
               c == '{' || c == '?' || c == '|';
    }

    std::string readWord() {
        size_t start = _pos;
        while (!atEnd()) {
            char c = peek();
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' ||
                c == '{' || c == '}' || c == '|' || c == '*' || c == '?') {
                break;
            }
            advance();
        }
        if (_pos == start) fail("empty literal token");
        return _text.substr(start, _pos - start);
    }

    PhraseAtom parseGroup() {
        advance();   // '('
        PhraseAtom atom;
        atom.kind = PhraseAtom::Kind::Group;
        atom.alternatives.push_back(parseSequence(/*insideGroup=*/true));
        while (true) {
            skipSpaces();
            if (peek() != '|') break;
            advance();
            atom.alternatives.push_back(parseSequence(/*insideGroup=*/true));
        }
        skipSpaces();
        if (peek() != ')') fail("unbalanced '(' - missing ')'");
        advance();
        return atom;
    }

    PhraseAtom parseBraceToken() {
        if (_pos + 1 < _text.size() && _text[_pos + 1] == '{') return parseCapture();
        return parseBarePlaceholder();
    }

    PhraseAtom parseCapture() {
        advance(); advance();   // '{{'
        size_t start = _pos;
        size_t close = _text.find("}}", _pos);
        if (close == std::string::npos) fail("unterminated '{{' capture - missing '}}'");
        std::string inner = _text.substr(start, close - start);
        _pos = close + 2;
        size_t colon = inner.find(':');
        if (colon == std::string::npos) {
            fail("capture \"{{" + inner + "}}\" is missing ':' - expected {{name:type}}");
        }
        PhraseAtom atom;
        atom.kind = PhraseAtom::Kind::Capture;
        atom.captureName = inner.substr(0, colon);
        atom.captureType = toLower(inner.substr(colon + 1));
        if (atom.captureName.empty() || atom.captureType.empty()) {
            fail("capture \"{{" + inner + "}}\" has an empty name or type");
        }
        return atom;
    }

    PhraseAtom parseBarePlaceholder() {
        advance();   // '{'
        size_t start = _pos;
        size_t close = _text.find('}', _pos);
        if (close == std::string::npos) fail("unterminated '{' placeholder - missing '}'");
        std::string inner = _text.substr(start, close - start);
        _pos = close + 1;
        if (inner.find(':') != std::string::npos) {
            fail("\"{" + inner + "}\" uses single braces with a ':' - did you mean {{" + inner + "}}?");
        }
        if (inner.empty()) fail("empty '{}' placeholder");
        PhraseAtom atom;
        atom.kind = PhraseAtom::Kind::BarePlaceholder;
        atom.canonical = inner;
        return atom;
    }
};

//  Entity dictionary compilation (spec §22, EntityDictionary/EntityGroup/EntityValue)

struct EntitySurfaceForm {
    std::string canonical;
    std::vector<std::string> words;   // tokenized, lowercased synonym
};

struct CompiledEntityDictionary {
    // group name (lowercased) -> surface forms, sorted longest-word-count-first
    std::unordered_map<std::string, std::vector<EntitySurfaceForm>> byGroup;
    // canonical name -> that one value's surface forms, sorted longest-first
    std::unordered_map<std::string, std::vector<EntitySurfaceForm>> byCanonical;
};

CompiledEntityDictionary compileDictionary(const EntityDictionary& dict) {
    CompiledEntityDictionary compiled;
    std::unordered_set<std::string> seenCanonicals;
    for (const auto& group : dict.groups()) {
        std::string groupKey = toLower(group.name());
        for (const auto& value : group.values()) {
            if (!seenCanonicals.insert(value.canonical()).second) {
                throw std::runtime_error("[PhraseGrammarMatcher] duplicate EntityValue.canonical '" +
                                          value.canonical() +
                                          "' - canonical names must be unique across the entire dictionary");
            }
            for (const auto& synonym : value.synonyms()) {
                EntitySurfaceForm form{value.canonical(), tokenizeWords(synonym)};
                compiled.byGroup[groupKey].push_back(form);
                compiled.byCanonical[value.canonical()].push_back(std::move(form));
            }
        }
    }
    auto byLengthDesc = [](const EntitySurfaceForm& a, const EntitySurfaceForm& b) {
        return a.words.size() > b.words.size();
    };
    for (auto& entry : compiled.byGroup) std::sort(entry.second.begin(), entry.second.end(), byLengthDesc);
    for (auto& entry : compiled.byCanonical) std::sort(entry.second.begin(), entry.second.end(), byLengthDesc);
    return compiled;
}

//  Compile-time validation - spec §22.1's "fail loudly, not at match time" rule

void collectCaptureNames(const std::vector<PhraseAtom>& atoms, std::vector<std::string>& names) {
    for (const auto& atom : atoms) {
        if (atom.kind == PhraseAtom::Kind::Capture) names.push_back(atom.captureName);
        if (atom.kind == PhraseAtom::Kind::Group) {
            for (const auto& branch : atom.alternatives) collectCaptureNames(branch, names);
        }
    }
}

void validateCaptureTypes(const std::vector<PhraseAtom>& atoms, const CompiledEntityDictionary& dict,
                           const std::string& phraseText) {
    for (const auto& atom : atoms) {
        if (atom.kind == PhraseAtom::Kind::Capture) {
            bool isGroupType = dict.byGroup.find(atom.captureType) != dict.byGroup.end();
            if (!isBuiltinParameterType(atom.captureType) && !isGroupType) {
                throw std::runtime_error("[PhraseGrammarMatcher] phrase \"" + phraseText + "\": capture type '" +
                                          atom.captureType + "' in {{" + atom.captureName + ":" + atom.captureType +
                                          "}} is neither a built-in Parameter type nor a declared EntityGroup name");
            }
        } else if (atom.kind == PhraseAtom::Kind::BarePlaceholder) {
            if (dict.byCanonical.find(atom.canonical) == dict.byCanonical.end()) {
                throw std::runtime_error("[PhraseGrammarMatcher] phrase \"" + phraseText + "\": {" + atom.canonical +
                                          "} does not resolve to any EntityValue.canonical in the dictionary");
            }
        } else if (atom.kind == PhraseAtom::Kind::Group) {
            for (const auto& branch : atom.alternatives) validateCaptureTypes(branch, dict, phraseText);
        }
    }
}

void validatePhrase(const CompiledPhrase& phrase, const CompiledEntityDictionary& dict) {
    validateCaptureTypes(phrase.atoms, dict, phrase.sourceText);
    std::vector<std::string> names;
    collectCaptureNames(phrase.atoms, names);
    std::unordered_set<std::string> seen;
    for (const auto& name : names) {
        if (!seen.insert(name).second) {
            throw std::runtime_error("[PhraseGrammarMatcher] phrase \"" + phrase.sourceText +
                                      "\": capture name '" + name + "' is used more than once in the same phrase");
        }
    }
}

//  Specificity scoring - spec §22.1: "fewer */. tokens and a higher literal-word
//  ratio score higher"; exact formula is an open decision (§18), this is a
//  documented, reasonable implementation of it.

void countAtoms(const std::vector<PhraseAtom>& atoms, int& literalCount, int& totalCount) {
    for (const auto& atom : atoms) {
        ++totalCount;
        if (atom.kind == PhraseAtom::Kind::Literal) ++literalCount;
        if (atom.kind == PhraseAtom::Kind::Group) {
            for (const auto& branch : atom.alternatives) countAtoms(branch, literalCount, totalCount);
        }
    }
}

float specificity(const CompiledPhrase& phrase) {
    int literalCount = 0, totalCount = 0;
    countAtoms(phrase.atoms, literalCount, totalCount);
    if (totalCount == 0) return 0.0f;
    return static_cast<float>(literalCount) / static_cast<float>(totalCount);
}

//  Utterance tokenization

std::vector<std::string> tokenizeUtterance(const std::string& utterance) {
    std::vector<std::string> raw = tokenizeWords(utterance);
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (const auto& w : raw) {
        // "20%" -> "20", "%" so a trailing literal "%" atom in a pattern (e.g.
        // "{{amount:number}}%") can match it as its own token.
        if (w.size() > 1 && w.back() == '%') {
            out.push_back(w.substr(0, w.size() - 1));
            out.push_back("%");
        } else {
            out.push_back(w);
        }
    }
    return out;
}

//  Backtracking matcher (continuation-passing)

using Captures = std::map<std::string, google::protobuf::Value>;
using Continuation = std::function<bool(size_t)>;

bool matchSeqThen(const std::vector<PhraseAtom>& atoms, size_t atomIdx, const std::vector<std::string>& words,
                   size_t wordIdx, Captures& captures, const CompiledEntityDictionary& dict,
                   const Continuation& cont);

bool tryParseNumber(const std::string& word, double& out) {
    if (word.empty()) return false;
    char* end = nullptr;
    out = std::strtod(word.c_str(), &end);
    return end != word.c_str() && *end == '\0';
}

bool matchEntitySpan(const std::vector<EntitySurfaceForm>& forms, const std::vector<std::string>& words,
                      size_t wordIdx, const std::function<bool(size_t, const std::string&)>& onMatch) {
    for (const auto& form : forms) {
        size_t n = form.words.size();
        if (n == 0 || wordIdx + n > words.size()) continue;
        bool matched = true;
        for (size_t i = 0; i < n; ++i) {
            if (words[wordIdx + i] != form.words[i]) { matched = false; break; }
        }
        if (matched && onMatch(wordIdx + n, form.canonical)) return true;
    }
    return false;
}

bool matchAtomThen(const PhraseAtom& atom, const std::vector<std::string>& words, size_t wordIdx,
                    Captures& captures, const CompiledEntityDictionary& dict, const Continuation& cont) {
    switch (atom.kind) {
        case PhraseAtom::Kind::Literal: {
            if (wordIdx < words.size() && words[wordIdx] == atom.literal && cont(wordIdx + 1)) return true;
            if (atom.optional && cont(wordIdx)) return true;
            return false;
        }
        case PhraseAtom::Kind::Wildcard: {
            for (size_t take = 0; wordIdx + take <= words.size(); ++take) {
                if (cont(wordIdx + take)) return true;
            }
            return false;
        }
        case PhraseAtom::Kind::DotWildcard: {
            if (wordIdx < words.size() && cont(wordIdx + 1)) return true;
            if (atom.optional && cont(wordIdx)) return true;
            return false;
        }
        case PhraseAtom::Kind::Capture: {
            auto groupIt = dict.byGroup.find(atom.captureType);
            if (groupIt != dict.byGroup.end()) {
                bool ok = matchEntitySpan(groupIt->second, words, wordIdx,
                    [&](size_t nextIdx, const std::string& canonical) {
                        google::protobuf::Value v;
                        v.set_string_value(canonical);
                        captures[atom.captureName] = v;
                        if (cont(nextIdx)) return true;
                        captures.erase(atom.captureName);
                        return false;
                    });
                if (ok) return true;
            } else if (wordIdx < words.size()) {
                const std::string& word = words[wordIdx];
                google::protobuf::Value v;
                double d;
                bool isNumeric = (atom.captureType == "number" || atom.captureType == "integer");
                if (isNumeric) {
                    if (tryParseNumber(word, d)) {
                        v.set_number_value(d);
                        captures[atom.captureName] = v;
                        if (cont(wordIdx + 1)) return true;
                        captures.erase(atom.captureName);
                    }
                } else {
                    v.set_string_value(word);
                    captures[atom.captureName] = v;
                    if (cont(wordIdx + 1)) return true;
                    captures.erase(atom.captureName);
                }
            }
            if (atom.optional && cont(wordIdx)) return true;
            return false;
        }
        case PhraseAtom::Kind::BarePlaceholder: {
            auto it = dict.byCanonical.find(atom.canonical);
            if (it != dict.byCanonical.end()) {
                bool ok = matchEntitySpan(it->second, words, wordIdx,
                    [&](size_t nextIdx, const std::string&) { return cont(nextIdx); });
                if (ok) return true;
            }
            if (atom.optional && cont(wordIdx)) return true;
            return false;
        }
        case PhraseAtom::Kind::Group: {
            for (const auto& branch : atom.alternatives) {
                if (matchSeqThen(branch, 0, words, wordIdx, captures, dict, cont)) return true;
            }
            if (atom.optional && cont(wordIdx)) return true;
            return false;
        }
    }
    return false;
}

bool matchSeqThen(const std::vector<PhraseAtom>& atoms, size_t atomIdx, const std::vector<std::string>& words,
                   size_t wordIdx, Captures& captures, const CompiledEntityDictionary& dict,
                   const Continuation& cont) {
    if (atomIdx == atoms.size()) return cont(wordIdx);
    return matchAtomThen(atoms[atomIdx], words, wordIdx, captures, dict,
        [&](size_t nextWordIdx) { return matchSeqThen(atoms, atomIdx + 1, words, nextWordIdx, captures, dict, cont); });
}

bool matchPhrase(const CompiledPhrase& phrase, const std::vector<std::string>& words,
                  const CompiledEntityDictionary& dict, Captures& captures) {
    return matchSeqThen(phrase.atoms, 0, words, 0, captures, dict,
        [&](size_t endIdx) { return endIdx == words.size(); });
}

struct CompiledIntent {
    std::string intentName;
    std::vector<CompiledPhrase> positive;
    std::vector<CompiledPhrase> negative;
};

}   // namespace

struct PhraseGrammarMatcher::Impl {
    CompiledEntityDictionary dictionary;
    std::vector<CompiledIntent> intents;
};

PhraseGrammarMatcher::PhraseGrammarMatcher(const TrainingSet& trainingSet) : _impl(std::make_shared<Impl>()) {
    _impl->dictionary = compileDictionary(trainingSet.entities());

    std::unordered_set<std::string> seenIntentNames;
    for (const auto& intentData : trainingSet.intents()) {
        if (!seenIntentNames.insert(intentData.intent_name()).second) {
            throw std::runtime_error("[PhraseGrammarMatcher] duplicate intent_name '" +
                                      intentData.intent_name() + "'");
        }
        CompiledIntent compiled;
        compiled.intentName = intentData.intent_name();
        for (const auto& phraseText : intentData.phrases()) {
            PhraseParser parser(phraseText);
            CompiledPhrase phrase = parser.parse();
            validatePhrase(phrase, _impl->dictionary);
            if (phrase.negative) {
                compiled.negative.push_back(std::move(phrase));
            } else {
                compiled.positive.push_back(std::move(phrase));
            }
        }
        _impl->intents.push_back(std::move(compiled));
    }
}

std::vector<PhraseGrammarMatcher::Match> PhraseGrammarMatcher::match(const std::string& utterance) const {
    std::vector<std::string> words = tokenizeUtterance(utterance);
    std::vector<Match> results;

    for (const auto& intent : _impl->intents) {
        bool negativeHit = false;
        for (const auto& neg : intent.negative) {
            Captures discard;
            if (matchPhrase(neg, words, _impl->dictionary, discard)) {
                negativeHit = true;
                break;
            }
        }
        if (negativeHit) continue;

        const CompiledPhrase* best = nullptr;
        Captures bestCaptures;
        float bestScore = -1.0f;
        for (const auto& phrase : intent.positive) {
            Captures captures;
            if (matchPhrase(phrase, words, _impl->dictionary, captures)) {
                float score = specificity(phrase);
                if (score > bestScore) {
                    bestScore = score;
                    best = &phrase;
                    bestCaptures = std::move(captures);
                }
            }
        }
        if (best != nullptr) {
            Match m;
            m.intent_name = intent.intentName;
            m.extracted_parameters = std::move(bestCaptures);
            m.confidence = bestScore;
            results.push_back(std::move(m));
        }
    }

    std::sort(results.begin(), results.end(),
              [](const Match& a, const Match& b) { return a.confidence > b.confidence; });
    return results;
}

}   // namespace orchestrator
}   // namespace va
