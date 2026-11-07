#include "flow/Entities.h"
#include "flow/FlowRegistry.h"
#include "nlu/Language.h"
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

// How alike a word must be to a value's, inside a span an entity model marked as that entity.
constexpr float kInSpanSimilarity = 0.6f;

bool isDigits(const std::string& word, bool decimals) {
    bool digit = false, point = false;
    for (const char c : word) {
        if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
        else if (c == '.' && decimals && !point) point = true;
        else return false;
    }
    return digit;
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

// What a language found, as matches of `entity`: text, a number, or yes/no.
enum class Kind { Text, Number, YesNo };
std::vector<EntityMatch> converted(const std::string& entity, const std::vector<nlu::Found>& found,
                                   const std::vector<std::string>& words, Kind kind) {
    std::vector<EntityMatch> matches;
    for (const auto& value : found) {
        const Value said = kind == Kind::Number ? numberValue(std::stod(value.value))
                         : kind == Kind::YesNo  ? boolValue(value.value == "true")
                                                : stringValue(value.value);
        auto match = matchOf(entity, said, words, value.start, value.length);
        match.spoken = value.spoken;
        matches.push_back(std::move(match));
    }
    return matches;
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

std::vector<std::string> entityWords(const std::string& said) {
    const std::string text = nlu::foldedText(said);
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

EntityCatalog::EntityCatalog() : _languages(std::make_shared<const nlu::Languages>()) {}

EntityCatalog::EntityCatalog(std::shared_ptr<const nlu::Languages> languages)
    : _languages(languages ? std::move(languages) : std::make_shared<const nlu::Languages>()) {}

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
                                                const DynamicEntityValues& dynamic, std::chrono::system_clock::time_point now,
                                                const std::vector<nlu::EntitySpan>* spans) const {
    // Values in any of the agent's languages, said either way: "next Tuesday", "half past three",
    // "john dot doe at gmail dot com", "demain".
    if (name.rfind("sys.", 0) == 0) {
        const auto& languages = *_languages;
        const auto today = nlu::dateAt(now);
        auto system = [&](auto find, Kind kind) { return converted(name, languages.each(find), words, kind); };
        if (name == "sys.date") return system([&](const nlu::Language& language) { return language.dates(words, today); }, Kind::Text);
        if (name == "sys.time") return system([&](const nlu::Language& language) { return language.times(words); }, Kind::Text);
        if (name == "sys.datetime")
            return system([&](const nlu::Language& language) { return language.dateTimes(words, today); }, Kind::Text);
        if (name == "sys.duration") return system([&](const nlu::Language& language) { return language.durations(words); }, Kind::Text);
        if (name == "sys.email") return system([&](const nlu::Language& language) { return language.emails(words); }, Kind::Text);
        if (name == "sys.ordinal") return system([&](const nlu::Language& language) { return language.ordinals(words); }, Kind::Number);
        if (name == "sys.integer")
            return system([&](const nlu::Language& language) { return language.numbers(words, false); }, Kind::Number);
        if (name == "sys.number")
            return system([&](const nlu::Language& language) { return language.numbers(words, true); }, Kind::Number);
        if (name == "sys.phone")
            return system([&](const nlu::Language& language) { return language.digitRuns(words, 7, 11); }, Kind::Text);
        if (name == "sys.digits")
            return system([&](const nlu::Language& language) { return language.digitRuns(words, 1, std::string::npos); }, Kind::Text);
        if (name == "sys.yes_no") return system([&](const nlu::Language& language) { return language.yesNo(words); }, Kind::YesNo);
    }
    const auto* entity = type(name);
    if (!entity) return {};
    std::vector<EntityMatch> found;
    if (entity->has_list()) {
        // Misspelled words match too ("tyres"), unless the entity says otherwise. With an entity
        // model, only inside the words it marked as this entity: "times" isn't "tires" anywhere else.
        const auto& matching = entity->matching();
        auto inSpan = [spans, &name](size_t start, size_t length) {
            return spans == nullptr || std::any_of(spans->begin(), spans->end(), [&](const nlu::EntitySpan& span) {
                return span.entity == name && start >= span.start && start + length <= span.start + span.length;
            });
        };
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
                auto spots = nlu::findPhrase(words, phraseWords, spans ? 1.0f : minSimilarity);
                auto add = [&](const std::pair<size_t, float>& spot) {
                    if (inSpan(spot.first, phraseWords.size()) &&
                        std::none_of(spots.begin(), spots.end(), [&](const auto& found) { return found.first == spot.first; }))
                        spots.push_back(spot);
                };
                // Inside the model's span the words are this entity: only which value is left to
                // decide, so a looser likeness does ("tyers" for "tyres"), fuzzy off excepted.
                if (spans && minSimilarity < 1.0f)
                    for (const auto& spot : nlu::findPhrase(words, phraseWords, std::min(minSimilarity, kInSpanSimilarity))) add(spot);
                if (matching.phonetic())   // "Catherine" for "Katherine", "Schmidt" for "Smith"
                    for (const auto& spot : soundsAt(phraseWords)) add(spot);
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
                    std::optional<std::string> spelled;   // "alpha" → "a", "five" → "5", "ab" → "ab"
                    for (const auto& language : _languages->all())
                        if ((spelled = language->spelledLetter(word))) break;
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
                    for (auto& candidate : matches(part, words, dynamic, now, spans))
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
        for (auto match : matches(withoutAt(derived.base()), words, dynamic, now, spans)) {
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
                                               const DynamicEntityValues& dynamic, std::chrono::system_clock::time_point now,
                                               const std::vector<nlu::EntitySpan>* spans) const {
    std::optional<EntityMatch> best;
    for (auto& match : matches(name, entityWords(text), dynamic, now, spans))
        if (!best || match.length > best->length || (match.length == best->length && match.start < best->start))
            best = std::move(match);
    return best;
}

std::vector<EntityMatch> EntityCatalog::findAll(const std::string& text, const DynamicEntityValues& dynamic,
                                                std::chrono::system_clock::time_point now,
                                                const std::vector<nlu::EntitySpan>* spans) const {
    const auto words = entityWords(text);
    std::vector<std::pair<size_t, EntityMatch>> candidates;   // declaration index, match
    for (size_t i = 0; i < _order.size(); ++i)
        for (auto& match : matches(_order[i], words, dynamic, now, spans)) candidates.emplace_back(i, std::move(match));
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

std::vector<nlu::EntityLabel> EntityCatalog::modelLabels() const {
    std::vector<nlu::EntityLabel> labels;
    for (const auto& name : _order) {
        const auto& type = _types.at(name);
        if (!type.has_list()) continue;
        nlu::EntityLabel label{name, type.matching().label(), type.description(), {}};
        if (label.label.empty()) {
            label.label = name;
            std::replace(label.label.begin(), label.label.end(), '_', ' ');
        }
        for (const auto& value : type.list().values()) {   // a few, as they're said
            if (label.examples.size() >= 12) break;
            label.examples.push_back(value.spoken().empty() ? value.value() : value.spoken());
            for (const auto& synonym : value.synonyms())
                if (label.examples.size() < 12) label.examples.push_back(synonym);
        }
        labels.push_back(std::move(label));
    }
    return labels;
}

EntityMatch EntityCatalog::describe(const std::string& name, const Value& value, const DynamicEntityValues& dynamic) const {
    EntityMatch match;
    match.entity = name;
    match.value = value;
    match.spoken = toText(value);
    std::vector<vasdk::nlu::EntityValue> declared;
    const auto* entity = type(name);
    const std::string base = entity && entity->has_derived() ? withoutAt(entity->derived().base()) : name;
    // A system value as a reply says it, in the agent's language: "Tuesday, November 10th", not 2026-11-10.
    if (value.kind_case() == Value::kStringValue && !_languages->empty()) {
        const auto& text = value.string_value();
        const auto& language = _languages->speaking();
        if (base == "sys.date") match.spoken = language.spokenDate(text);
        else if (base == "sys.time") match.spoken = language.spokenTime(text);
        else if (base == "sys.duration") match.spoken = language.spokenDuration(text);
        else if (base == "sys.datetime") match.spoken = language.spokenDateTime(text);
        else if (base == "sys.yes_no") match.spoken = text;
        if (match.spoken.empty()) match.spoken = text;
    } else if (value.kind_case() == Value::kBoolValue && base == "sys.yes_no" && !_languages->empty()) {
        const auto& pack = _languages->speaking().pack();   // as it was said: "yes"
        const auto& words = value.bool_value() ? pack.yes() : pack.no();
        if (!words.empty()) match.spoken = words.Get(0);
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
