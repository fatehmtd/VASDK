#include "flow/ExamplePattern.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>

namespace va {
namespace flow {

namespace {

struct PatternError {
    std::string message;
};

using Resolve = std::function<std::vector<std::string>(const std::string& reference)>;

bool nameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool namePart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Reads one pattern, spelling its sentences as it goes. Text is kept as written: words are
// split later, by what reads the examples.
class PatternParser {
public:
    PatternParser(const std::string& text, const Resolve& resolve, size_t limit) : _text(text), _resolve(resolve), _limit(limit) {}

    std::vector<std::string> all() {
        auto spelled = sequence();
        if (_at < _text.size()) fail(std::string("unmatched '") + _text[_at] + "'");
        return spelled;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw PatternError{"column " + std::to_string(_at + 1) + ": " + message};
    }

    // Each of `left` followed by each of `right`.
    std::vector<std::string> join(const std::vector<std::string>& left, const std::vector<std::string>& right) const {
        if (left.size() * right.size() > _limit)
            fail("spells more than " + std::to_string(_limit) + " sentences: split it, or use fewer alternatives");
        std::vector<std::string> joined;
        joined.reserve(left.size() * right.size());
        for (const auto& first : left)
            for (const auto& second : right) joined.push_back(first + second);
        return joined;
    }

    // Up to the end, or the ')', ']' or '|' that ends an alternative.
    std::vector<std::string> sequence() {
        std::vector<std::string> spelled{""};
        while (_at < _text.size()) {
            const char c = _text[_at];
            if (c == ')' || c == ']' || c == '|') break;
            if (c == '\\') {
                if (_at + 1 >= _text.size()) fail("a '\\' with nothing after it");
                for (auto& sentence : spelled) sentence += _text[_at + 1];
                _at += 2;
            } else if (c == '(' || c == '[') {
                ++_at;
                auto choices = alternatives(c == '(' ? ')' : ']');
                if (c == '[') choices.push_back("");
                spelled = join(spelled, choices);
            } else if (c == '$' && _at + 1 < _text.size() && _text[_at + 1] == '{') {
                const size_t start = _at + 2;
                const size_t end = _text.find('}', start);
                if (end == std::string::npos) fail("a '${' with no '}'");
                const std::string reference = _text.substr(start, end - start);
                spelled = join(spelled, _resolve(reference));
                _at = end + 1;
            } else {
                for (auto& sentence : spelled) sentence += c;
                ++_at;
            }
        }
        return spelled;
    }

    std::vector<std::string> alternatives(char close) {
        const size_t open = _at - 1;
        std::vector<std::string> choices;
        while (true) {
            auto choice = sequence();
            choices.insert(choices.end(), choice.begin(), choice.end());
            if (choices.size() > _limit) fail("spells more than " + std::to_string(_limit) + " sentences");
            if (_at >= _text.size()) {
                _at = open;
                fail(std::string("a '") + _text[open] + "' with no '" + close + "'");
            }
            const char c = _text[_at++];
            if (c == '|') continue;
            if (c == close) return choices;
            --_at;
            fail(std::string("a '") + c + "' where a '" + close + "' was expected");
        }
    }

    const std::string& _text;
    const Resolve& _resolve;
    size_t _limit;
    size_t _at = 0;
};

// Spaces collapsed, the empty ones dropped, each once, in order.
std::vector<std::string> tidy(const std::vector<std::string>& sentences) {
    std::vector<std::string> tidied;
    std::set<std::string> seen;
    for (const auto& sentence : sentences) {
        std::string clean;
        for (char c : sentence) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!clean.empty() && clean.back() != ' ') clean += ' ';
            } else {
                clean += c;
            }
        }
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (!clean.empty() && seen.insert(clean).second) tidied.push_back(std::move(clean));
    }
    return tidied;
}

std::string scopeLabel(const PhraseScope& scope) {
    switch (scope.kind) {
    case PhraseScope::Kind::Intent: return "the intent's";
    case PhraseScope::Kind::File: return "the file's";
    case PhraseScope::Kind::Pack: return "pack \"" + scope.name + "\"'s";
    }
    return "";
}

} // namespace

bool isPhraseSetName(const std::string& name) {
    if (name.empty() || !nameStart(name[0])) return false;
    return std::all_of(name.begin(), name.end(), namePart);
}

std::vector<std::string> sampleExamples(const std::vector<std::string>& examples, size_t most) {
    if (examples.size() <= most || most == 0) return most == 0 ? std::vector<std::string>{} : examples;
    std::vector<std::string> sample;
    sample.reserve(most);
    for (size_t i = 0; i < most; ++i) sample.push_back(examples[i * examples.size() / most]);
    return sample;
}

bool isWildcard(const std::string& word) { return word == "*" || word == "+" || word == "_"; }

std::string withoutWildcards(const std::string& example) {
    std::string plain;
    std::string word;
    std::istringstream words(example);
    while (words >> word)
        if (!isWildcard(word)) plain += (plain.empty() ? "" : " ") + word;
    return plain;
}

std::vector<std::string> plainExamples(const std::vector<std::string>& examples, size_t most) {
    std::vector<std::string> plain;
    std::set<std::string> seen;
    for (const auto& example : sampleExamples(examples, most)) {
        auto words = withoutWildcards(example);
        if (!words.empty() && seen.insert(words).second) plain.push_back(std::move(words));
    }
    return plain;
}

std::optional<std::vector<std::string>> expandPattern(const std::string& pattern, std::string* error, size_t limit) {
    const Resolve none = [](const std::string& reference) -> std::vector<std::string> {
        throw PatternError{"${" + reference + "}: no phrase sets here"};
    };
    try {
        return tidy(PatternParser(pattern, none, limit).all());
    } catch (const PatternError& problem) {
        if (error) *error = problem.message;
        return std::nullopt;
    }
}

ExampleExpander::ExampleExpander(std::vector<std::shared_ptr<const PhraseScope>> scopes, size_t limit)
    : _scopes(std::move(scopes)), _limit(limit) {}

ExampleExpander::Found ExampleExpander::find(const Chain& chain, const std::string& reference, size_t from) const {
    const auto dot = reference.find('.');
    const std::string pack = dot == std::string::npos ? "" : reference.substr(0, dot);
    const std::string name = dot == std::string::npos ? reference : reference.substr(dot + 1);
    if ((dot != std::string::npos && !isPhraseSetName(pack)) || !isPhraseSetName(name))
        throw PatternError{"${" + reference + "}: a set's name is letters, digits and _ (a pack's set: ${pack.name})"};
    auto in = [&](size_t index) -> std::optional<Found> {
        const auto found = chain[index]->sets.find(name);
        if (found == chain[index]->sets.end()) return std::nullopt;
        return Found{index, &found->first, &found->second};
    };
    // A pack's sets see only that pack.
    if (chain[from]->kind == PhraseScope::Kind::Pack) {
        if (!pack.empty() && pack != chain[from]->name)
            throw PatternError{"${" + reference + "}: a pack's sets use only its own (pack \"" + chain[from]->name + "\")"};
        if (auto found = in(from)) return *found;
        throw PatternError{"${" + reference + "}: pack \"" + chain[from]->name + "\" has no set \"" + name + "\""};
    }
    if (!pack.empty()) {
        for (size_t index = from; index < chain.size(); ++index) {
            if (chain[index]->kind != PhraseScope::Kind::Pack || chain[index]->name != pack) continue;
            if (auto found = in(index)) return *found;
            throw PatternError{"${" + reference + "}: pack \"" + pack + "\" has no set \"" + name + "\""};
        }
        throw PatternError{"${" + reference + "}: this file uses no pack \"" + pack + "\" (phrase_set_files)"};
    }
    for (size_t index = from; index < chain.size(); ++index)
        if (chain[index]->kind != PhraseScope::Kind::Pack)
            if (auto found = in(index)) return *found;
    std::vector<Found> inPacks;
    for (size_t index = from; index < chain.size(); ++index)
        if (chain[index]->kind == PhraseScope::Kind::Pack)
            if (auto found = in(index)) inPacks.push_back(*found);
    if (inPacks.size() > 1) {
        std::string packs;
        for (const auto& found : inPacks) packs += (packs.empty() ? "" : ", ") + ("${" + chain[found.index]->name + "." + name + "}");
        throw PatternError{"${" + reference + "}: more than one pack has it: say which, " + packs};
    }
    if (inPacks.size() == 1) return inPacks.front();
    throw PatternError{"${" + reference + "}: no phrase set \"" + name + "\" here (the intent's, the file's or a pack it uses)"};
}

std::vector<std::string> ExampleExpander::spellSet(const Chain& chain, const Found& found) {
    const auto key = std::make_pair(chain[found.index], *found.name);
    _used.insert(key);
    if (const auto spelled = _spelled.find(key); spelled != _spelled.end()) return spelled->second;
    if (std::find(_expanding.begin(), _expanding.end(), key) != _expanding.end()) {
        std::string loop;
        bool inLoop = false;
        for (const auto& [scope, name] : _expanding) {
            inLoop = inLoop || (scope == key.first && name == key.second);
            if (inLoop) loop += name + " → ";
        }
        throw PatternError{"phrase sets refer to each other in a loop: " + loop + key.second};
    }
    _expanding.push_back(key);
    std::vector<std::string> all;
    for (const auto& phrase : *found.phrases) {
        std::vector<std::string> spelled;
        try {
            spelled = spell(chain, phrase, found.index);
        } catch (const PatternError& problem) {
            if (problem.message.rfind("phrase sets refer", 0) == 0) throw;
            throw PatternError{scopeLabel(*chain[found.index]) + " set \"" + *found.name + "\" phrase \"" + phrase + "\": " + problem.message};
        }
        all.insert(all.end(), spelled.begin(), spelled.end());
        if (all.size() > _limit)
            throw PatternError{"set \"" + *found.name + "\" spells more than " + std::to_string(_limit) + " sentences"};
    }
    _expanding.pop_back();
    auto tidied = tidy(all);
    _spelled[key] = tidied;
    return tidied;
}

std::vector<std::string> ExampleExpander::spell(const Chain& chain, const std::string& text, size_t from) {
    const Resolve resolve = [&](const std::string& reference) { return spellSet(chain, find(chain, reference, from)); };
    return PatternParser(text, resolve, _limit).all();
}

std::optional<std::vector<std::string>> ExampleExpander::expand(const std::string& pattern, const PhraseScope* intentSets,
                                                                std::string* error) {
    Chain chain;
    if (intentSets) chain.push_back(intentSets);
    for (const auto& scope : _scopes) chain.push_back(scope.get());
    _expanding.clear();
    try {
        if (chain.empty()) return expandPattern(pattern, error, _limit);
        return tidy(spell(chain, pattern, 0));
    } catch (const PatternError& problem) {
        if (error) *error = problem.message;
        return std::nullopt;
    }
}

bool ExampleExpander::used(const PhraseScope& scope, const std::string& name) const { return _used.count({&scope, name}) > 0; }

bool ExampleExpander::usedAny(const PhraseScope& scope) const {
    return std::any_of(_used.begin(), _used.end(), [&scope](const auto& key) { return key.first == &scope; });
}

std::vector<std::string> ExampleExpander::hidingNotes(const PhraseScope* intentSets) const {
    Chain chain;
    if (intentSets) chain.push_back(intentSets);
    for (const auto& scope : _scopes) chain.push_back(scope.get());
    std::vector<std::string> notes;
    for (size_t near = 0; near < chain.size(); ++near) {
        if (chain[near]->kind == PhraseScope::Kind::Pack) continue;   // packs don't hide each other: ambiguity is an error
        for (const auto& [name, phrases] : chain[near]->sets)
            for (size_t far = near + 1; far < chain.size(); ++far)
                if (chain[far]->sets.count(name))
                    notes.push_back(chain[near]->where + ": its phrase set \"" + name + "\" hides " + scopeLabel(*chain[far]));
    }
    return notes;
}

} // namespace flow
} // namespace va
