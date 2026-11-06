#include "flow/Entities.h"
#include "flow/FlowRegistry.h"
#include "nlu/English.h"
#include "nlu/NluProvider.h"

#include <algorithm>
#include <sstream>
#include <regex>
#include <cctype>
#include <cstdlib>
#include <set>
#include <spdlog/spdlog.h>

namespace va {
namespace flow {

namespace {

std::string inQuotes(const std::string& text) { return "\"" + text + "\""; }

bool isDigits(const std::string& word, bool decimals) {
    bool digit = false, point = false;
    for (const char c : word) {
        if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
        else if (c == '.' && decimals && !point) point = true;
        else return false;
    }
    return digit;
}

const std::map<std::string, int>& units() {   // 0-19
    static const std::map<std::string, int> words{
        {"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7},
        {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13},
        {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}};
    return words;
}

const std::map<std::string, int>& tens() {
    static const std::map<std::string, int> words{{"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
                                                  {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90}};
    return words;
}

// A digit said or written: "five", "oh", "5".
std::optional<char> digitOf(const std::string& word) {
    if (word.size() == 1 && std::isdigit(static_cast<unsigned char>(word[0]))) return word[0];
    if (word == "oh") return '0';
    const auto unit = units().find(word);
    if (unit != units().end() && unit->second < 10) return static_cast<char>('0' + unit->second);
    return std::nullopt;
}

EntityMatch matchOf(const std::string& entity, Value value, const std::vector<std::string>& words, size_t start,
                    size_t length) {
    EntityMatch match;
    match.entity = entity;
    match.spoken = toText(value);
    match.value = std::move(value);
    for (size_t i = start; i < start + length; ++i) match.raw += (i > start ? " " : "") + words[i];
    match.start = start;
    match.length = length;
    return match;
}

// Numbers in digits ("25", "2.5") or words ("twenty five", "a hundred and two"); "five five" is two.
std::vector<EntityMatch> numbers(const std::string& entity, const std::vector<std::string>& words, bool decimals) {
    std::vector<EntityMatch> found;
    for (size_t i = 0; i < words.size();) {
        if (isDigits(words[i], decimals)) {
            found.push_back(matchOf(entity, numberValue(std::strtod(words[i].c_str(), nullptr)), words, i, 1));
            ++i;
            continue;
        }
        long long total = 0, current = 0;
        size_t end = i, last = i;
        bool any = false, unitLast = false;
        for (size_t j = i; j < words.size(); ++j) {
            const auto& word = words[j];
            const auto unit = units().find(word);
            const auto ten = tens().find(word);
            if (unit != units().end()) {
                if (unitLast || (any && current % 10 != 0)) break;   // "five five": a new number
                current += unit->second;
                unitLast = true;
            } else if (ten != tens().end()) {
                if (any && current % 100 != 0) break;
                current += ten->second;
                unitLast = false;
            } else if (word == "hundred") {
                current = std::max<long long>(current, 1) * 100;
                unitLast = false;
            } else if (word == "thousand") {
                total += std::max<long long>(current, 1) * 1000;
                current = 0;
                unitLast = false;
            } else if ((word == "and" && any) || (word == "a" && j + 1 < words.size() &&
                                                  (words[j + 1] == "hundred" || words[j + 1] == "thousand"))) {
                continue;
            } else {
                break;
            }
            any = true;
            last = j;
            end = j + 1;
        }
        if (!any) {
            ++i;
            continue;
        }
        double number = static_cast<double>(total + current);
        if (decimals && end + 1 < words.size() && words[end] == "point") {   // "twenty three point five"
            std::string fraction;
            size_t after = end + 1;
            for (; after < words.size(); ++after) {
                const auto digit = digitOf(words[after]);
                if (!digit) break;
                fraction += *digit;
            }
            if (!fraction.empty()) {
                number += std::stod("0." + fraction);
                last = after - 1;
                end = after;
            }
        }
        found.push_back(matchOf(entity, numberValue(number), words, i, last - i + 1));
        i = end;
    }
    return found;
}

// Runs of digits, written or said one by one ("double four" too): "5551234567". sys.phone keeps
// the runs of 7 to 11; sys.digits any.
std::vector<EntityMatch> digitRuns(const std::string& entity, const std::vector<std::string>& words, size_t fewest,
                                   size_t most) {
    std::vector<EntityMatch> found;
    for (size_t i = 0; i < words.size();) {
        std::string digits;
        size_t j = i;
        int repeat = 1;
        for (; j < words.size(); ++j) {
            if (words[j] == "double" || words[j] == "triple") {
                repeat = words[j] == "double" ? 2 : 3;
                continue;
            }
            if (isDigits(words[j], false)) {
                for (int r = 0; r < repeat; ++r) digits += words[j];
            } else if (const auto digit = digitOf(words[j])) {
                digits.append(static_cast<size_t>(repeat), *digit);
            } else {
                break;
            }
            repeat = 1;
        }
        if (digits.size() >= fewest && digits.size() <= most) found.push_back(matchOf(entity, stringValue(digits), words, i, j - i));
        i = j > i ? j : i + 1;
    }
    return found;
}

std::vector<EntityMatch> yesNo(const std::vector<std::string>& words) {
    static const std::map<std::string, bool> answers{
        {"yes", true}, {"yeah", true}, {"yep", true}, {"yup", true}, {"sure", true}, {"correct", true},
        {"right", true}, {"ok", true}, {"okay", true}, {"absolutely", true}, {"definitely", true},
        {"true", true}, {"no", false}, {"nope", false}, {"nah", false}, {"not", false}, {"wrong", false},
        {"incorrect", false}, {"don't", false}, {"false", false}};
    std::vector<EntityMatch> found;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto answer = answers.find(words[i]);
        if (answer == answers.end()) continue;
        auto match = matchOf("sys.yes_no", boolValue(answer->second), words, i, 1);
        match.spoken = answer->second ? "yes" : "no";
        found.push_back(std::move(match));
    }
    return found;
}

// A word as spelled out: "alpha" → "a", "five" → "5", "ab" → "ab".
std::optional<std::string> spelledOf(const std::string& word) {
    static const std::map<std::string, char> alphabet{
        {"alpha", 'a'}, {"alfa", 'a'}, {"bravo", 'b'}, {"charlie", 'c'}, {"delta", 'd'}, {"echo", 'e'},
        {"foxtrot", 'f'}, {"golf", 'g'}, {"hotel", 'h'}, {"india", 'i'}, {"juliet", 'j'}, {"juliett", 'j'},
        {"kilo", 'k'}, {"lima", 'l'}, {"mike", 'm'}, {"november", 'n'}, {"oscar", 'o'}, {"papa", 'p'},
        {"quebec", 'q'}, {"romeo", 'r'}, {"sierra", 's'}, {"tango", 't'}, {"uniform", 'u'}, {"victor", 'v'},
        {"whiskey", 'w'}, {"xray", 'x'}, {"yankee", 'y'}, {"zulu", 'z'}};
    const auto letter = alphabet.find(word);
    if (letter != alphabet.end()) return std::string(1, letter->second);
    if (const auto digit = digitOf(word)) return std::string(1, *digit);
    // Letters and digits written as heard ("AB 123" from the STT) are kept as they are.
    if (std::all_of(word.begin(), word.end(), [](unsigned char c) { return c < 0x80 && std::isalnum(c); })) return word;
    return std::nullopt;
}

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

bool isSystem(const std::string& name) {
    return std::any_of(std::begin(kSystemEntities), std::end(kSystemEntities), [&](const char* system) { return name == system; });
}

std::string withoutAt(const std::string& reference) { return !reference.empty() && reference[0] == '@' ? reference.substr(1) : reference; }

} // namespace

std::vector<std::string> entityWords(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        // A written email stays one word: "john.doe@gmail.com".
        if (word.empty() && (i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])))) {
            static const std::regex email("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}");
            std::smatch found;
            const std::string rest = text.substr(i, text.find_first_of(" \t\r\n", i) - i);
            if (std::regex_search(rest, found, email)) {
                std::string address = found[0].str();
                std::transform(address.begin(), address.end(), address.begin(), [](unsigned char c) { return std::tolower(c); });
                words.push_back(std::move(address));
                i += found[0].length() - 1;
                continue;
            }
        }
        if (std::isalnum(byte) || text[i] == '\'' || byte >= 0x80) {
            word += static_cast<char>(std::tolower(byte));
        } else if ((text[i] == '.' || text[i] == ':') && !word.empty() && std::isdigit(static_cast<unsigned char>(word.back())) &&
                   i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            word += text[i];   // "2.5", "3:30"
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

void EntityCatalog::build(const google::protobuf::RepeatedPtrField<vasdk::nlu::EntityType>& types,
                          std::vector<std::string>& errors, std::shared_ptr<const FlowRegistry> registry) {
    _registry = registry;
    for (int i = 0; i < types.size(); ++i) {
        const auto& type = types.Get(i);
        const std::string where = "entity " + (type.name().empty() ? std::to_string(i + 1) : inQuotes(type.name()));
        auto error = [&](const std::string& message) { errors.push_back(where + ": " + message); };
        if (type.name().empty()) { error("has no name"); continue; }
        if (type.name().rfind("sys.", 0) == 0) { error("names starting with sys. are reserved"); continue; }
        if (!_types.emplace(type.name(), type).second) { error("is declared twice"); continue; }
        _order.push_back(type.name());
        if (type.dynamic() && !type.has_list()) error("only list entities can be dynamic");
        switch (type.kind_case()) {
        case vasdk::nlu::EntityType::kList: {
            std::map<std::string, std::string> phrases;   // phrase → the value it belongs to
            for (const auto& value : type.list().values()) {
                if (value.value().empty()) error("a value is empty");
                std::vector<std::string> said(value.synonyms().begin(), value.synonyms().end());
                said.push_back(value.value());
                for (const auto& phrase : said) {
                    std::string joined;
                    for (const auto& word : entityWords(phrase)) joined += (joined.empty() ? "" : " ") + word;
                    const auto [at, added] = phrases.emplace(joined, value.value());
                    if (!added && at->second != value.value())
                        error(inQuotes(phrase) + " is a synonym of both " + inQuotes(at->second) + " and " + inQuotes(value.value()));
                }
            }
            if (type.list().values().empty() && !type.dynamic()) error("has no values");
            break;
        }
        case vasdk::nlu::EntityType::kPattern:
            try {
                _patterns[type.name()] = std::make_shared<std::regex>(type.pattern().regex(),
                                                                      std::regex::ECMAScript | std::regex::icase);
            } catch (const std::regex_error& regexError) {
                error("regex " + inQuotes(type.pattern().regex()) + ": " + regexError.what());
            }
            if (type.pattern().regex().empty()) error("pattern has no regex");
            break;
        case vasdk::nlu::EntityType::kDerived:
        case vasdk::nlu::EntityType::kComposite:
            break;   // checked below, once every name is known
        case vasdk::nlu::EntityType::kCode:
            if (type.code().extractor().empty()) error("code needs an extractor");
            else if (!registry || !registry->entityExtractor(type.code().extractor()))
                error("extractor " + inQuotes(type.code().extractor()) + " isn't registered (FlowRegistry::addEntityExtractor)");
            break;
        case vasdk::nlu::EntityType::KIND_NOT_SET:
            error("needs a list, pattern, derived, composite or code kind");
            break;
        }
    }
    for (const auto& name : _order) {   // a composite's parts: known entities, each filling a field once
        const auto& type = _types.at(name);
        if (!type.has_composite()) continue;
        const std::string where = "entity " + inQuotes(name);
        if (type.composite().templates().empty()) errors.push_back(where + ": composite has no templates");
        for (const auto& text : type.composite().templates()) {
            std::set<std::string> fields;
            bool anyPart = false;
            std::istringstream tokens(text);
            std::string token;
            while (tokens >> token) {
                if (token[0] != '@') continue;
                anyPart = true;
                const auto colon = token.find(':');
                const std::string part = token.substr(1, colon == std::string::npos ? std::string::npos : colon - 1);
                const std::string field = colon == std::string::npos ? std::string() : token.substr(colon + 1);
                const auto partType = _types.find(part);
                if (!isSystem(part) && (partType == _types.end() || partType->second.has_composite()))
                    errors.push_back(where + ": template " + inQuotes(text) + ": @" + part + " must be a system, list, pattern or derived entity");
                if (field.empty()) errors.push_back(where + ": template " + inQuotes(text) + ": @" + part + " needs a field: @" + part + ":<field>");
                else if (!fields.insert(field).second) errors.push_back(where + ": template " + inQuotes(text) + " fills " + inQuotes(field) + " twice");
            }
            if (!anyPart) errors.push_back(where + ": template " + inQuotes(text) + " has no @entity parts");
        }
    }
    for (const auto& name : _order) {
        const auto& type = _types.at(name);
        if (!type.has_derived()) continue;
        const std::string where = "entity " + inQuotes(name);
        const std::string base = withoutAt(type.derived().base());
        const auto baseType = _types.find(base);
        if (!isSystem(base) && (baseType == _types.end() || baseType->second.has_derived()))
            errors.push_back(where + ": base " + inQuotes(type.derived().base()) +
                             " must be @sys.integer, @sys.number, @sys.phone, @sys.digits, @sys.yes_no or a list or pattern entity");
        if (type.derived().condition().empty()) continue;
        std::string parseError;
        auto condition = Expression::parse(type.derived().condition(), &parseError);
        if (!condition) {
            errors.push_back(where + ": condition " + inQuotes(type.derived().condition()) + ": " + parseError);
            continue;
        }
        for (const auto& path : condition->references())
            if (path[0] != "value") errors.push_back(where + ": condition reads " + pathText(path) + "; it can read only $value");
        _conditions[name] = std::move(condition);
    }
}

bool EntityCatalog::knows(const std::string& name) const { return isSystem(name) || _types.count(name); }

const vasdk::nlu::EntityType* EntityCatalog::type(const std::string& name) const {
    const auto found = _types.find(name);
    return found == _types.end() ? nullptr : &found->second;
}

const std::vector<vasdk::nlu::EntityValue>* EntityCatalog::listValues(const std::string& name,
                                                                      const DynamicEntityValues& dynamic,
                                                                      std::vector<vasdk::nlu::EntityValue>& declared) const {
    const auto* entity = type(name);
    if (!entity || !entity->has_list()) return nullptr;
    const auto replaced = dynamic.find(name);
    if (entity->dynamic() && replaced != dynamic.end()) return &replaced->second;
    declared.assign(entity->list().values().begin(), entity->list().values().end());
    return &declared;
}

int EntityCatalog::kindRank(const std::string& name) const {
    const auto* entity = type(name);
    if (!entity) return 4;
    return entity->has_pattern() ? 0 : entity->has_list() || entity->has_code() ? 1 : entity->has_derived() ? 2 : 3;
}

std::vector<EntityMatch> EntityCatalog::matches(const std::string& name, const std::vector<std::string>& words,
                                                const DynamicEntityValues& dynamic, std::chrono::system_clock::time_point now) const {
    // English values said either way: "next Tuesday", "half past three", "john dot doe at gmail dot com".
    auto english = [&](const std::vector<nlu::english::Found>& found, bool number) {
        std::vector<EntityMatch> converted;
        for (const auto& value : found) {
            auto match = matchOf(name, number ? numberValue(std::stod(value.value)) : stringValue(value.value), words, value.start,
                                 value.length);
            match.spoken = value.spoken;
            converted.push_back(std::move(match));
        }
        return converted;
    };
    if (name == "sys.date") return english(nlu::english::dates(words, nlu::english::dateAt(now)), false);
    if (name == "sys.time") return english(nlu::english::times(words), false);
    if (name == "sys.datetime") return english(nlu::english::dateTimes(words, nlu::english::dateAt(now)), false);
    if (name == "sys.duration") return english(nlu::english::durations(words), false);
    if (name == "sys.email") return english(nlu::english::emails(words), false);
    if (name == "sys.ordinal") return english(nlu::english::ordinals(words), true);
    if (name == "sys.integer") return numbers(name, words, false);
    if (name == "sys.number") return numbers(name, words, true);
    if (name == "sys.phone") return digitRuns(name, words, 7, 11);
    if (name == "sys.digits") return digitRuns(name, words, 1, std::string::npos);
    if (name == "sys.yes_no") return yesNo(words);
    const auto* entity = type(name);
    if (!entity) return {};
    std::vector<EntityMatch> found;
    if (entity->has_list()) {
        // Misspelled words match too ("tyres"), unless the entity says otherwise.
        const auto& matching = entity->matching();
        const float minSimilarity = matching.has_fuzzy() && !matching.fuzzy() ? 1.0f
                                  : matching.has_min_similarity()           ? matching.min_similarity()
                                                                            : 0.8f;
        // Where `phrase` is said word for word by sound (Double Metaphone), for names STT spells its own way.
        auto soundsAt = [&words](const std::vector<std::string>& phrase) {
            std::vector<std::pair<size_t, float>> spots;
            for (size_t start = 0; start + phrase.size() <= words.size(); ++start) {
                size_t i = 0;
                while (i < phrase.size() && (words[start + i] == phrase[i] ||
                                             // 3 letters or more: "all" isn't the name "Al"
                                             (words[start + i].size() >= 3 && phrase[i].size() >= 3 &&
                                              nlu::soundsAlike(words[start + i], phrase[i]))))
                    ++i;
                if (i == phrase.size()) spots.emplace_back(start, static_cast<float>(phrase.size()));
            }
            return spots;
        };
        std::vector<vasdk::nlu::EntityValue> declared;
        for (const auto& value : *listValues(name, dynamic, declared)) {
            std::vector<std::string> said(value.synonyms().begin(), value.synonyms().end());
            said.push_back(value.value());
            if (!value.spoken().empty()) said.push_back(value.spoken());
            for (const auto& phrase : said) {
                const auto phraseWords = entityWords(phrase);
                if (phraseWords.empty()) continue;
                auto spots = nlu::findPhrase(words, phraseWords, minSimilarity);
                if (matching.phonetic()) {   // "Catherine" for "Katherine", "Schmidt" for "Smith"
                    for (const auto& spot : soundsAt(phraseWords))
                        if (std::none_of(spots.begin(), spots.end(), [&](const auto& found) { return found.first == spot.first; }))
                            spots.push_back(spot);
                }
                for (const auto& [start, similarity] : spots) {
                    auto match = matchOf(name, stringValue(value.value()), words, start, phraseWords.size());
                    match.spoken = value.spoken().empty() ? value.value() : value.spoken();
                    match.attributes = value.attributes();
                    found.push_back(std::move(match));
                }
            }
        }
    } else if (entity->has_pattern()) {
        const auto& pattern = entity->pattern();
        const auto regex = _patterns.find(name);
        if (regex == _patterns.end()) return {};
        constexpr size_t kMaxWords = 16;
        for (size_t start = 0; start < words.size(); ++start) {
            std::string candidate;
            std::optional<EntityMatch> longest;
            for (size_t length = 1; length <= kMaxWords && start + length <= words.size(); ++length) {
                const auto& word = words[start + length - 1];
                if (pattern.spelled()) {
                    const auto spelled = spelledOf(word);
                    if (!spelled) break;
                    candidate += *spelled;
                } else {
                    candidate += (length > 1 && !pattern.strip_spaces() ? " " : "") + word;
                }
                if (std::regex_match(candidate, *regex->second))
                    longest = matchOf(name, stringValue(pattern.uppercase() ? upper(candidate) : candidate), words, start, length);
            }
            if (longest) found.push_back(std::move(*longest));
        }
    } else if (entity->has_code()) {
        const auto* extractor = _registry ? _registry->entityExtractor(entity->code().extractor()) : nullptr;
        if (!extractor) return {};
        std::string text;
        for (const auto& word : words) text += (text.empty() ? "" : " ") + word;
        std::vector<FlowRegistry::EntityFound> values;
        try {
            values = (*extractor)(FlowRegistry::EntityRequest{words, text});
        } catch (const std::exception& failure) {
            spdlog::warn("[Entities] extractor \"{}\" failed: {}", entity->code().extractor(), failure.what());
        }
        for (auto& value : values) {
            if (value.length == 0 || value.start + value.length > words.size()) continue;   // outside the words
            auto match = matchOf(name, value.value, words, value.start, value.length);
            match.spoken = value.spoken.empty() ? toText(value.value) : value.spoken;
            match.attributes = std::move(value.attributes);
            found.push_back(std::move(match));
        }
    } else if (entity->has_composite()) {
        // Each template in order from each word: its words said as they are, each part a match
        // of its entity starting right there (the longest).
        for (const auto& text : entity->composite().templates()) {
            std::vector<std::string> tokens;
            std::istringstream split(text);
            for (std::string token; split >> token;) tokens.push_back(token);
            for (size_t start = 0; start < words.size(); ++start) {
                size_t cursor = start;
                google::protobuf::Struct fields;
                std::string spoken;
                bool matched = true;
                for (const auto& token : tokens) {
                    if (token[0] != '@') {
                        for (const auto& literal : entityWords(token)) {
                            if (cursor >= words.size() || nlu::wordMatch(words[cursor], literal, 0.8f) == 0.0f) {
                                matched = false;
                                break;
                            }
                            spoken += (spoken.empty() ? "" : " ") + words[cursor++];
                        }
                        if (!matched) break;
                        continue;
                    }
                    const auto colon = token.find(':');
                    const std::string part = token.substr(1, colon - 1), field = token.substr(colon + 1);
                    std::optional<EntityMatch> best;
                    for (auto& candidate : matches(part, words, dynamic, now))
                        if (candidate.start == cursor && (!best || candidate.length > best->length)) best = std::move(candidate);
                    if (!best) {
                        matched = false;
                        break;
                    }
                    (*fields.mutable_fields())[field] = best->value;
                    spoken += (spoken.empty() ? "" : " ") + best->spoken;
                    cursor += best->length;
                }
                if (!matched || cursor == start) continue;
                Value value;
                *value.mutable_struct_value() = std::move(fields);
                auto match = matchOf(name, std::move(value), words, start, cursor - start);
                match.spoken = spoken;
                found.push_back(std::move(match));
            }
        }
    } else if (entity->has_derived()) {
        const auto& derived = entity->derived();
        const auto condition = _conditions.find(name);
        for (auto match : matches(withoutAt(derived.base()), words, dynamic, now)) {
            if (match.value.kind_case() == Value::kNumberValue) {
                if (derived.has_min() && match.value.number_value() < derived.min()) continue;
                if (derived.has_max() && match.value.number_value() > derived.max()) continue;
            }
            if (condition != _conditions.end() &&
                !condition->second->test([&](const Path& path) { return valueAt(match.value, path, 1); }, now))
                continue;
            match.entity = name;
            found.push_back(std::move(match));
        }
    }
    return found;
}

std::optional<EntityMatch> EntityCatalog::find(const std::string& name, const std::string& text,
                                               const DynamicEntityValues& dynamic, std::chrono::system_clock::time_point now) const {
    std::optional<EntityMatch> best;
    for (auto& match : matches(name, entityWords(text), dynamic, now))
        if (!best || match.length > best->length || (match.length == best->length && match.start < best->start))
            best = std::move(match);
    return best;
}

std::vector<EntityMatch> EntityCatalog::findAll(const std::string& text, const DynamicEntityValues& dynamic,
                                                std::chrono::system_clock::time_point now) const {
    const auto words = entityWords(text);
    std::vector<std::pair<size_t, EntityMatch>> candidates;   // declaration index, match
    for (size_t i = 0; i < _order.size(); ++i)
        for (auto& match : matches(_order[i], words, dynamic, now)) candidates.emplace_back(i, std::move(match));
    std::stable_sort(candidates.begin(), candidates.end(), [this](const auto& left, const auto& right) {
        if (left.second.length != right.second.length) return left.second.length > right.second.length;
        const int leftRank = kindRank(left.second.entity), rightRank = kindRank(right.second.entity);
        if (leftRank != rightRank) return leftRank < rightRank;
        return left.first < right.first;
    });
    std::vector<EntityMatch> accepted;
    std::vector<bool> taken(words.size(), false);
    for (auto& [index, match] : candidates) {
        const bool free = std::none_of(taken.begin() + static_cast<long>(match.start),
                                       taken.begin() + static_cast<long>(match.start + match.length), [](bool used) { return used; });
        if (!free) continue;
        std::fill(taken.begin() + static_cast<long>(match.start), taken.begin() + static_cast<long>(match.start + match.length), true);
        accepted.push_back(std::move(match));
    }
    std::sort(accepted.begin(), accepted.end(), [](const auto& left, const auto& right) { return left.start < right.start; });
    return accepted;
}

EntityMatch EntityCatalog::describe(const std::string& name, const Value& value, const DynamicEntityValues& dynamic) const {
    EntityMatch match;
    match.entity = name;
    match.value = value;
    match.spoken = toText(value);
    std::vector<vasdk::nlu::EntityValue> declared;
    const auto* entity = type(name);
    const std::string base = entity && entity->has_derived() ? withoutAt(entity->derived().base()) : name;
    // A system value as a reply says it: "Tuesday, November 10th", not 2026-11-10.
    if (value.kind_case() == Value::kStringValue) {
        const auto& text = value.string_value();
        if (base == "sys.date") match.spoken = nlu::english::spokenDate(text);
        else if (base == "sys.time") match.spoken = nlu::english::spokenTime(text);
        else if (base == "sys.duration") match.spoken = nlu::english::spokenDuration(text);
        else if (base == "sys.datetime" && text.find('T') != std::string::npos) {
            const auto day = nlu::english::spokenDate(text.substr(0, text.find('T')));
            const auto time = nlu::english::spokenTime(text.substr(text.find('T') + 1));
            if (!day.empty() && !time.empty()) match.spoken = day + " at " + time;
        }
        if (match.spoken.empty()) match.spoken = text;
    }
    if (const auto* values = listValues(base, dynamic, declared)) {
        for (const auto& candidate : *values) {
            if (candidate.value() != toText(value)) continue;
            if (!candidate.spoken().empty()) match.spoken = candidate.spoken();
            match.attributes = candidate.attributes();
            break;
        }
    }
    return match;
}

std::vector<vasdk::nlu::EntityValue> EntityCatalog::values(const std::string& name, const DynamicEntityValues& dynamic) const {
    std::vector<vasdk::nlu::EntityValue> declared;
    const auto* values = listValues(name, dynamic, declared);
    return values ? *values : std::vector<vasdk::nlu::EntityValue>();
}

std::vector<std::string> EntityCatalog::spokenValues(const std::string& name, const DynamicEntityValues& dynamic) const {
    std::vector<vasdk::nlu::EntityValue> declared;
    std::vector<std::string> spoken;
    if (const auto* values = listValues(name, dynamic, declared))
        for (const auto& value : *values) spoken.push_back(value.spoken().empty() ? value.value() : value.spoken());
    return spoken;
}

Value entityField(const EntityMatch& match, const Path& path, size_t rest, bool spoken) {
    if (rest >= path.size()) return spoken ? stringValue(match.spoken) : match.value;
    const auto& field = path[rest];
    if (field == "value") return valueAt(match.value, path, rest + 1);
    if (field == "spoken") return stringValue(match.spoken);
    if (field == "raw") return stringValue(match.raw);
    if (field == "entity") return stringValue(match.entity);
    const auto attribute = match.attributes.fields().find(field);
    return attribute == match.attributes.fields().end() ? nullValue() : valueAt(attribute->second, path, rest + 1);
}

} // namespace flow
} // namespace va
