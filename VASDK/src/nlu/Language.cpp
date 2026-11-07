#include "nlu/Language.h"

#include "engine/ProtoFileLoader.h"
#include "flow/Entities.h"
#include "flow/Expression.h"

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace va {
namespace nlu {

// languages/en-US.txtpb, built into the library (generated: BuiltInLanguage.cpp).
std::string builtInLanguagePack();

namespace {

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

bool isDigits(const std::string& word) {
    return !word.empty() && std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c); });
}

template <class T, class Value>
void addPhrase(PhraseTable<T>& table, const std::string& phrase, Value value) {
    table.add(flow::entityWords(phrase), static_cast<T>(value));
}

template <class Range>
void addPhrases(PhraseTable<bool>& table, const Range& phrases) {
    for (const auto& phrase : phrases) table.add(flow::entityWords(phrase), true);
}

// A one-word entry as entityWords() would split it: "Don't" → "don't"; empty for none or several words.
std::string oneWord(const std::string& text) {
    const auto words = flow::entityWords(text);
    return words.size() == 1 ? words.front() : std::string();
}

Found found(std::string value, std::string spoken, size_t start, size_t end) {
    Found result;
    result.value = std::move(value);
    result.spoken = std::move(spoken);
    result.start = start;
    result.length = end - start;
    return result;
}

std::string clock(int hour, int minute) {
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
    return text;
}

// A number as a value's text: "25", "2.5".
std::string numberText(double number) {
    char text[32];
    if (std::floor(number) == number && std::fabs(number) < 1e15) std::snprintf(text, sizeof(text), "%.0f", number);
    else std::snprintf(text, sizeof(text), "%.10g", number);
    return text;
}

// The power of ten of a number's first digit: 25 → 10, 7 → 1, 300 → 100.
long long placeOf(long long value) {
    long long place = 1;
    while (place * 10 <= value) place *= 10;
    return place;
}

// A month and day without a year: the next one from today.
Date upcoming(Date today, int month, int day) {
    Date date{today.year, month, day};
    if (daysOf(date) < daysOf(today)) date.year += 1;
    return date;
}

// "{weekday}, {month} {day}" with its values in place; an unknown name stays as it is.
std::string filled(std::string format, const std::vector<std::pair<std::string, std::string>>& values) {
    for (const auto& [name, value] : values) {
        const std::string key = "{" + name + "}";
        for (size_t at = format.find(key); at != std::string::npos; at = format.find(key, at + value.size()))
            format.replace(at, key.size(), value);
    }
    return format;
}

std::string trimmed(const std::string& text) {
    const auto start = text.find_first_not_of(' ');
    if (start == std::string::npos) return {};
    return text.substr(start, text.find_last_not_of(' ') - start + 1);
}

// "2026-11-10" → its parts; false when it isn't that shape.
bool isoParts(const std::string& text, int& year, int& month, int& day) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (const size_t at : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!std::isdigit(static_cast<unsigned char>(text[at]))) return false;
    year = std::stoi(text.substr(0, 4));
    month = std::stoi(text.substr(5, 2));
    day = std::stoi(text.substr(8, 2));
    return true;
}

// "15:30" → its parts; false when it isn't that shape.
bool clockParts(const std::string& text, int& hour, int& minute) {
    if (text.size() != 5 || text[2] != ':') return false;
    for (const size_t at : {0, 1, 3, 4})
        if (!std::isdigit(static_cast<unsigned char>(text[at]))) return false;
    hour = std::stoi(text.substr(0, 2));
    minute = std::stoi(text.substr(3, 2));
    return hour <= 23 && minute <= 59;
}

std::string escapedForRegex(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ? std::string(1, c) : std::string("\\") + c;
}

// A code point in lower case: Latin-1, Latin Extended-A, Greek and Cyrillic; others as they are.
uint32_t lowerCodePoint(uint32_t c) {
    if (c < 0x80) return static_cast<uint32_t>(std::tolower(static_cast<int>(c)));
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;                  // À-Þ
    if (c == 0x130) return 'i';                                                // İ
    if (c == 0x178) return 0xFF;                                               // Ÿ
    if ((c >= 0x100 && c <= 0x137 && c != 0x131) || (c >= 0x14A && c <= 0x177))
        return c % 2 == 0 ? c + 1 : c;                                         // Ā ā ... (upper even)
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c % 2 == 1 ? c + 1 : c;   // Ĺ ĺ ... (upper odd)
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 0x20;               // Greek capitals
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;                             // Cyrillic А-Я
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;                             // Ѐ-Џ
    return c;
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

} // namespace

std::string foldedText(const std::string& text) {
    std::string out;
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
        if (!valid) {   // not UTF-8: the byte as it is
            out += text[i++];
            continue;
        }
        if (c == 0x2019 || c == 0x2018) c = '\'';   // ’ ‘
        appendUtf8(out, lowerCodePoint(c));
        i += length;
    }
    return out;
}

Language::Language(vasdk::nlu::Language pack, std::vector<std::string>* errors) : _pack(std::move(pack)) {
    auto problem = [&](const std::string& text) {
        if (errors) errors->push_back("language " + quoted(_pack.code()) + ": " + text);
    };
    if (_pack.code().empty()) problem("has no code");

    const auto& numbers = _pack.numbers();
    for (const auto& [phrase, value] : numbers.words()) addPhrase(_numberWords, phrase, value);
    for (const auto& [phrase, value] : numbers.scales()) {
        if (value < 10) problem("scale " + quoted(phrase) + " must be 10 or more");
        addPhrase(_scales, phrase, value);
    }
    addPhrases(_connectors, numbers.connectors());
    addPhrases(_ones, numbers.ones());
    addPhrases(_decimalPoints, numbers.decimal_points());
    for (const auto& [phrase, value] : numbers.digit_aliases()) {
        if (value < 0 || value > 9) problem("digit alias " + quoted(phrase) + " must be 0 to 9");
        addPhrase(_digitAliases, phrase, value);
    }
    for (const auto& [phrase, value] : numbers.repeats()) addPhrase(_repeats, phrase, value);
    for (const auto& [phrase, value] : numbers.ordinals()) addPhrase(_ordinals, phrase, value);
    if (!numbers.ordinal_suffix().empty()) {
        try {
            _ordinalSuffix.emplace("^([0-9]{1,2})(" + numbers.ordinal_suffix() + ")$", std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error& error) {
            problem("ordinal_suffix " + quoted(numbers.ordinal_suffix()) + ": " + error.what());
        }
    }
    addPhrases(_ordinalLeads, numbers.ordinal_leads());
    addPhrases(_ordinalTrailers, numbers.ordinal_trailers());
    addPhrases(_articles, _pack.articles());

    const auto& calendar = _pack.calendar();
    if (calendar.weekdays_size() != 0 && calendar.weekdays_size() != 7) problem("calendar needs 7 weekdays, Monday first");
    for (int day = 0; day < calendar.weekdays_size(); ++day) {
        if (calendar.weekdays(day).phrases().empty()) problem("weekday " + std::to_string(day + 1) + " has no name");
        for (const auto& phrase : calendar.weekdays(day).phrases()) addPhrase(_weekdays, phrase, day);
    }
    if (calendar.months_size() != 0 && calendar.months_size() != 12) problem("calendar needs 12 months, January first");
    for (int month = 0; month < calendar.months_size(); ++month) {
        if (calendar.months(month).phrases().empty()) problem("month " + std::to_string(month + 1) + " has no name");
        for (const auto& phrase : calendar.months(month).phrases()) addPhrase(_months, phrase, month + 1);
    }
    for (const auto& [phrase, days] : calendar.relative_days()) addPhrase(_relativeDays, phrase, days);
    addPhrases(_fromToday, calendar.from_today());
    addPhrases(_dayUnits, calendar.day_units());
    addPhrases(_weekUnits, calendar.week_units());
    addPhrases(_next, calendar.next());
    addPhrases(_coming, calendar.coming());
    addPhrases(_dayOfMonth, calendar.day_of_month());

    const auto& clockWords = _pack.clock();
    addPhrases(_noon, clockWords.noon());
    addPhrases(_midnight, clockWords.midnight());
    addPhrases(_am, clockWords.am());
    addPhrases(_pm, clockWords.pm());
    addPhrases(_at, clockWords.at());
    addPhrases(_hourWords, clockWords.hour_words());
    for (const auto& [phrase, minutes] : clockWords.fractions()) addPhrase(_fractions, phrase, minutes);
    addPhrases(_past, clockWords.past());
    addPhrases(_to, clockWords.to());
    for (const auto& [phrase, minutes] : clockWords.after_hour()) {
        if (minutes <= -60 || minutes >= 60) problem("after_hour " + quoted(phrase) + " must be -59 to 59 minutes");
        addPhrase(_afterHour, phrase, minutes);
    }
    addPhrases(_zero, clockWords.zero());
    addPhrases(_minus, clockWords.minus());

    const auto& durations = _pack.durations();
    for (const auto& [phrase, seconds] : durations.units()) addPhrase(_durationUnits, phrase, seconds);
    for (const auto& [phrase, amount] : durations.amounts()) addPhrase(_amounts, phrase, amount);
    addPhrases(_andAHalf, durations.and_a_half());
    addPhrases(_durationJoiners, durations.joiners());

    addPhrases(_emailAt, _pack.emails().at());
    for (const auto& [phrase, symbol] : _pack.emails().symbols())
        if (const auto word = oneWord(phrase); !word.empty()) _emailSymbols[word] = symbol;

    for (const auto& phrase : _pack.yes()) addPhrase(_yesNo, phrase, true);
    for (const auto& phrase : _pack.no()) addPhrase(_yesNo, phrase, false);
    for (const auto& word : _pack.negations())
        if (const auto said = oneWord(word); !said.empty()) _negations.insert(said);
    for (const auto& word : _pack.stop_words())
        if (const auto said = oneWord(word); !said.empty()) _stopWords.insert(said);
    for (const auto& [word, letter] : _pack.spelling_alphabet())
        if (const auto said = oneWord(word); !said.empty()) _spelling[said] = letter;

    const auto& speaking = _pack.speaking();
    auto rule = [&](const std::string& condition, std::string text, const std::string& where) {
        std::string parseError;
        auto expression = flow::Expression::parse(condition, &parseError);
        if (!expression) problem(where + " " + quoted(condition) + ": " + parseError);
        return Rule{std::move(expression), std::move(text)};
    };
    for (const auto& suffix : speaking.ordinal_suffixes())
        _ordinalSuffixes.push_back(rule(suffix.condition(), suffix.suffix(), "ordinal_suffixes condition"));
    for (const auto& condition : speaking.plural_rules()) _pluralRules.push_back(rule(condition, "", "plural_rules"));
    const int forms = speaking.plural_rules_size() + 1;
    for (const auto& [unit, said] : speaking.units())
        if (said.phrases_size() != 1 && said.phrases_size() != forms)
            problem("speaking unit " + quoted(unit) + " has " + std::to_string(said.phrases_size()) + " forms; plural_rules make " +
                    std::to_string(forms));
    if (_pack.replies().too_few_digits_size() > 1 && _pack.replies().too_few_digits_size() != forms)
        problem("replies too_few_digits has " + std::to_string(_pack.replies().too_few_digits_size()) + " forms; plural_rules make " +
                std::to_string(forms));
    if (speaking.digits_size() != 0 && speaking.digits_size() != 10) problem("speaking needs 10 digits, 0 to 9");

    // Text a voice reads: a date, a phone number, a 24-hour time (not one followed by am or pm).
    std::string periods;
    for (const auto* list : {&clockWords.am(), &clockWords.pm()}) {
        for (const auto& phrase : *list) {
            std::string letters;
            for (const auto& word : flow::entityWords(phrase)) letters += word;
            if (letters.empty() || letters.size() > 4) continue;
            std::string pattern;
            for (size_t i = 0; i < letters.size(); ++i) pattern += (i ? "[.\\s]?" : "") + escapedForRegex(letters[i]);
            periods += (periods.empty() ? "" : "|") + pattern;
        }
    }
    _speakable = std::regex("\\b([0-9]{4}-[0-9]{2}-[0-9]{2})\\b"                       // a date
                            "|\\(?\\b([0-9]{3})\\)?[-. ]([0-9]{3})[-. ]([0-9]{4})\\b"  // a phone number
                            "|\\b([01]?[0-9]|2[0-3]):([0-5][0-9])\\b" +               // a 24-hour time
                                (periods.empty() ? std::string() : "(?!\\s*(?:" + periods + "))"),
                            std::regex::ECMAScript | std::regex::icase);
}

Language::~Language() = default;

std::shared_ptr<const Language> Language::compile(vasdk::nlu::Language pack, std::string* error) {
    std::vector<std::string> errors;
    auto language = std::make_shared<const Language>(std::move(pack), &errors);
    if (errors.empty()) return language;
    if (error) {
        error->clear();
        for (const auto& line : errors) *error += (error->empty() ? "" : "\n") + line;
    }
    return nullptr;
}

std::shared_ptr<const Language> Language::load(const std::string& path, std::string* error) {
    vasdk::nlu::Language pack;
    if (!engine::loadProtoFile(path, pack, error, false)) return nullptr;
    std::string compileError;
    auto language = compile(std::move(pack), &compileError);
    if (!language && error) *error = path + ": " + compileError;
    return language;
}

std::string Language::builtInText() { return builtInLanguagePack(); }

std::shared_ptr<const Language> Language::builtIn() {
    vasdk::nlu::Language pack;
    google::protobuf::TextFormat::ParseFromString(builtInLanguagePack(), &pack);
    return std::make_shared<const Language>(std::move(pack));
}

bool Language::serves(const std::string& code) const {
    auto primary = [](const std::string& text) {
        std::string lower = text.substr(0, text.find_first_of("-_"));
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return lower;
    };
    return !code.empty() && primary(code) == primary(_pack.code());
}

bool Language::holds(const Rule& rule, double n) const {
    if (!rule.condition) return false;
    return rule.condition->test([n](const flow::Path& path) {
        return path.size() == 1 && path[0] == "n" ? flow::numberValue(n) : flow::nullValue();
    });
}

size_t Language::pluralForm(double count) const {
    for (size_t i = 0; i < _pluralRules.size(); ++i)
        if (holds(_pluralRules[i], count)) return i;
    return _pluralRules.size();
}

std::string Language::plural(const std::vector<std::string>& forms, double count) const {
    if (forms.empty()) return {};
    return forms[std::min(pluralForm(count), forms.size() - 1)];
}

std::optional<long long> Language::wordNumber(const std::vector<std::string>& words, size_t at, size_t& length,
                                              bool scales) const {
    long long total = 0, current = 0, last = 0, scaleLimit = 0;
    bool any = false;
    size_t end = at;
    auto applyScale = [&](long long scale) {
        if (scale >= 1000) {
            total += std::max<long long>(current, 1) * scale;
            current = 0;
        } else {
            current = std::max<long long>(current, 1) * scale;
        }
        scaleLimit = scale;
        last = 0;
        any = true;
    };
    for (size_t j = at; j < words.size();) {
        if (any) {   // "a hundred and two": a connector, when a number goes on after it
            if (const size_t connector = _connectors.length(words, j)) {
                const size_t next = j + connector;
                if (_numberWords.length(words, next) || (scales && _scales.length(words, next))) {
                    j = next;
                    continue;
                }
                break;
            }
        }
        if (scales) {
            if (const size_t one = _ones.length(words, j)) {   // "a hundred"
                if (const auto scale = _scales.at(words, j + one); scale && (!any || current == 0)) {
                    applyScale(scale->first);
                    j += one + scale->second;
                    end = j;
                    continue;
                }
            }
            if (const auto scale = _scales.at(words, j)) {
                if (scaleLimit && scale->first <= scaleLimit && current == 0) break;   // "hundred hundred"
                applyScale(scale->first);
                j += scale->second;
                end = j;
                continue;
            }
        }
        const auto word = _numberWords.at(words, j);
        if (!word) break;
        const long long value = word->first;
        if (any) {
            if (value == 0) break;
            if (scaleLimit) {
                if (value >= scaleLimit) break;            // after "hundred", a smaller part
            } else {
                const long long place = placeOf(value) * 10;   // "twenty" "five": 5 fills the ones of 20
                if (last % place != 0 || last <= place) break;   // "five five", "ten five", "twenty thirty": a new number
            }
        }
        current += value;
        last = value;
        scaleLimit = 0;
        any = true;
        j += word->second;
        end = j;
    }
    if (!any) return std::nullopt;
    length = end - at;
    return total + current;
}

std::optional<int> Language::smallNumber(const std::vector<std::string>& words, size_t at, size_t& length) const {
    if (at >= words.size()) return std::nullopt;
    if (isDigits(words[at]) && words[at].size() <= 2) {
        length = 1;
        return std::stoi(words[at]);
    }
    size_t taken = 0;
    const auto number = wordNumber(words, at, taken, false);
    if (!number || *number >= 100) return std::nullopt;
    length = taken;
    return static_cast<int>(*number);
}

std::optional<int> Language::ordinalNumber(const std::vector<std::string>& words, size_t at, size_t& length) const {
    if (at >= words.size()) return std::nullopt;
    std::smatch digits;
    if (_ordinalSuffix && std::regex_match(words[at], digits, *_ordinalSuffix)) {   // "5th"
        length = 1;
        return std::stoi(digits[1].str());
    }
    if (const auto ordinal = _ordinals.at(words, at)) {
        length = ordinal->second;
        return ordinal->first;
    }
    // "twenty first": a round number, then an ordinal below 10
    if (const auto tens = _numberWords.at(words, at); tens && tens->first >= 20 && tens->first % 10 == 0) {
        size_t next = at + tens->second;
        next += _connectors.length(words, next);
        if (const auto unit = _ordinals.at(words, next); unit && unit->first >= 1 && unit->first <= 9) {
            length = next + unit->second - at;
            return static_cast<int>(tens->first) + unit->first;
        }
    }
    return std::nullopt;
}

std::vector<Found> Language::dates(const std::vector<std::string>& words, Date today) const {
    std::vector<Found> result;
    auto add = [&](Date date, size_t start, size_t end) { result.push_back(found(iso(date), spokenDate(iso(date)), start, end)); };
    const bool nextIsComing = _pack.calendar().next_is_coming();
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& word = words[i];
        size_t length = 0;
        // 2026-11-10, as entityWords() splits it
        if (word.size() == 4 && isDigits(word) && i + 2 < words.size() && isDigits(words[i + 1]) && isDigits(words[i + 2]) &&
            validDate(std::stoi(word), std::stoi(words[i + 1]), std::stoi(words[i + 2]))) {
            add({std::stoi(word), std::stoi(words[i + 1]), std::stoi(words[i + 2])}, i, i + 3);
            i += 2;
            continue;
        }
        // "today", "tomorrow", "day after tomorrow", "next week"
        if (const auto relative = _relativeDays.at(words, i)) {
            add(addDays(today, relative->first), i, i + relative->second);
            i += relative->second - 1;
            continue;
        }
        // "in two weeks", "in a week", "in 3 days"
        if (const size_t from = _fromToday.length(words, i)) {
            const size_t amountAt = i + from;
            std::optional<int> count;
            if (const size_t one = _ones.length(words, amountAt)) {
                count = 1;
                length = one;
            } else {
                count = smallNumber(words, amountAt, length);
            }
            const size_t unitAt = amountAt + length;
            if (count) {
                const size_t dayUnit = _dayUnits.length(words, unitAt), weekUnit = dayUnit ? 0 : _weekUnits.length(words, unitAt);
                if (dayUnit || weekUnit) {
                    add(addDays(today, *count * (dayUnit ? 1 : 7)), i, unitAt + dayUnit + weekUnit);
                    i = unitAt + dayUnit + weekUnit - 1;
                    continue;
                }
            }
        }
        // "next Tuesday" (the Tuesday of next week), "mardi prochain"; "Tuesday", "this Tuesday": the next one from today
        {
            bool next = false;
            size_t before = _next.length(words, i);
            if (before) next = true;
            else before = _coming.length(words, i);
            if (const auto weekday = _weekdays.at(words, i + before)) {
                size_t end = i + before + weekday->second;
                if (!before)
                    if (const size_t after = _next.length(words, end)) {
                        next = true;
                        end += after;
                    }
                const long todayDays = daysOf(today);
                const int sundayFirst = (weekday->first + 1) % 7;
                long ahead = (sundayFirst - weekdayOf(todayDays) + 7) % 7;
                if (ahead == 0) ahead = 7;
                if (next && !nextIsComing) {
                    const long weekStart = todayDays - (weekdayOf(todayDays) + 6) % 7;   // Monday
                    if (todayDays + ahead < weekStart + 7) ahead += 7;                   // still this week: the one after
                }
                add(addDays(today, ahead), i, end);
                i = end - 1;
                continue;
            }
        }
        // "November 10th", "nov 10", "November tenth 2027"
        if (const auto month = _months.at(words, i)) {
            const size_t dayAt = i + month->second;
            std::optional<int> day = ordinalNumber(words, dayAt, length);
            if (!day) day = smallNumber(words, dayAt, length);
            if (day && validDate(today.year, month->first, *day)) {
                size_t end = dayAt + length;
                Date date = upcoming(today, month->first, *day);
                if (end < words.size() && words[end].size() == 4 && isDigits(words[end])) date.year = std::stoi(words[end++]);
                add(date, i, end);
                i = end - 1;
                continue;
            }
        }
        // "the fifth", "on the fifth", "the 10th of November", "10 November", "le 10 novembre"
        size_t ordinalAt = i;
        if (const size_t coming = _coming.length(words, ordinalAt); coming && _articles.length(words, ordinalAt + coming))
            ordinalAt += coming;
        ordinalAt += _articles.length(words, ordinalAt);
        std::optional<int> day = ordinalNumber(words, ordinalAt, length);
        const bool spelledOrdinal = day.has_value();
        if (!day) day = smallNumber(words, ordinalAt, length);
        if (day && *day >= 1 && *day <= 31) {
            const size_t end = ordinalAt + length;
            const size_t monthAt = end + _dayOfMonth.length(words, end);
            if (const auto month = _months.at(words, monthAt); month && validDate(today.year, month->first, *day)) {
                size_t last = monthAt + month->second;
                Date date = upcoming(today, month->first, *day);
                if (last < words.size() && words[last].size() == 4 && isDigits(words[last])) date.year = std::stoi(words[last++]);
                add(date, i, last);
                i = last - 1;
                continue;
            }
            if (spelledOrdinal && ordinalAt > i) {   // "the fifth": this month, or next once it's past
                Date date{today.year, today.month, *day};
                if (*day < today.day) date = Date{today.month == 12 ? today.year + 1 : today.year, today.month % 12 + 1, *day};
                if (validDate(date.year, date.month, date.day)) {
                    add(date, i, end);
                    i = end - 1;
                    continue;
                }
            }
        }
    }
    return result;
}

size_t Language::period(const std::vector<std::string>& words, size_t at, int& hour) const {
    if (const size_t pm = _pm.length(words, at)) {
        if (hour < 12) hour += 12;
        return pm;
    }
    if (const size_t am = _am.length(words, at)) {
        if (hour == 12) hour = 0;
        return am;
    }
    return 0;
}

std::vector<Found> Language::times(const std::vector<std::string>& words) const {
    std::vector<Found> result;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& word = words[i];
        size_t start = i;   // "at 9": the word before is part of it
        for (size_t back = 1; back <= std::min<size_t>(i, 3); ++back)
            if (_at.length(words, i - back) == back) {
                start = i - back;
                break;
            }
        auto add = [&](int hour, int minute, size_t end) {
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
            result.push_back(found(clock(hour, minute), spokenTime(clock(hour, minute)), start, end));
            i = end - 1;
            return true;
        };
        if (const size_t noon = _noon.length(words, i)) { add(12, 0, i + noon); continue; }
        if (const size_t midnight = _midnight.length(words, i)) { add(0, 0, i + midnight); continue; }
        std::smatch parts;
        if (std::regex_match(word, parts, _writtenTime) && (parts[2].matched || parts[3].length() > 0)) {
            int hour = std::stoi(parts[1].str());
            const int minute = parts[2].matched ? std::stoi(parts[2].str()) : 0;
            const std::vector<std::string> suffix{parts[3].str()};
            if (suffix[0].empty()) {
                const size_t taken = period(words, i + 1, hour);
                add(hour, minute, i + 1 + taken);
            } else if (period(suffix, 0, hour) == 1 || _hourWords.length(suffix, 0) == 1) {
                add(hour, minute, i + 1);
            }
            continue;
        }
        // "half past three", "quarter to five", "twenty past six"
        size_t length = 0;
        std::optional<int> minutes;
        if (const auto fraction = _fractions.at(words, i)) {
            minutes = fraction->first;
            length = fraction->second;
        } else {
            minutes = smallNumber(words, i, length);
        }
        if (minutes) {
            const size_t linkAt = i + length;
            const size_t past = _past.length(words, linkAt), to = past ? 0 : _to.length(words, linkAt);
            size_t hourLength = 0;
            if (past || to)
                if (auto hour = smallNumber(words, linkAt + past + to, hourLength); hour && *hour >= 1 && *hour <= 12) {
                    int h = *hour, m = *minutes;
                    if (to) {
                        h = h == 1 ? 12 : h - 1;
                        m = 60 - m;
                    }
                    size_t end = linkAt + past + to + hourLength;
                    end += _hourWords.length(words, end);
                    const size_t taken = period(words, end, h);
                    add(h, m, end + taken);
                    continue;
                }
        }
        // "three thirty pm", "9 am", "three o'clock", "at 9", "nine oh five", "trois heures et quart"
        size_t hourLength = 0;
        const auto hour = smallNumber(words, i, hourLength);
        if (!hour || *hour > 23) continue;
        size_t end = i + hourLength;
        int h = *hour, minute = 0;
        bool shaped = start < i;
        const size_t hourWord = _hourWords.length(words, end);
        if (hourWord) {
            end += hourWord;
            shaped = true;
        }
        size_t minuteLength = 0;
        if (const auto after = _afterHour.at(words, end)) {   // "et quart", "moins le quart"
            minute = after->first;
            if (minute < 0) {
                minute += 60;
                h = (h + 23) % 24;
            }
            end += after->second;
            shaped = true;
        } else if (const size_t zero = _zero.length(words, end); zero && end + zero < words.size()) {   // "nine oh five"
            if (const auto m = smallNumber(words, end + zero, minuteLength); m && *m < 10) {
                minute = *m;
                end += zero + minuteLength;
                shaped = true;
            }
        } else if (const size_t to = _minus.length(words, end)) {   // "cinq heures moins dix"
            std::optional<int> before;
            if (const auto fraction = _fractions.at(words, end + to)) {
                before = fraction->first;
                minuteLength = fraction->second;
            } else {
                before = smallNumber(words, end + to, minuteLength);
            }
            if (before && *before > 0 && *before < 60) {
                minute = 60 - *before;
                h = (h + 23) % 24;
                end += to + minuteLength;
            }
        } else if (const auto m = smallNumber(words, end, minuteLength); m && (*m >= 10 || hourWord) && *m < 60) {   // "three thirty", "trois heures cinq"
            minute = *m;
            end += minuteLength;
            shaped = true;
        }
        const size_t taken = period(words, end, h);
        if (taken) shaped = true;
        if (!shaped || (taken == 0 && *hour > 12 && minute == 0 && start == i && !hourWord)) continue;
        add(h, minute, end + taken);
    }
    return result;
}

std::vector<Found> Language::dateTimes(const std::vector<std::string>& words, Date today) const {
    const auto onDays = dates(words, today);
    const auto atTimes = times(words);
    if (onDays.empty() || atTimes.empty()) return {};
    const auto& date = onDays.front();
    const auto& time = atTimes.front();
    const size_t start = std::min(date.start, time.start);
    const size_t end = std::max(date.start + date.length, time.start + time.length);
    const std::string value = date.value + "T" + time.value;
    const std::string spoken = spokenDateTime(value);
    return {found(value, spoken.empty() ? value : spoken, start, end)};
}

std::vector<Found> Language::durations(const std::vector<std::string>& words) const {
    std::vector<Found> result;
    for (size_t i = 0; i < words.size(); ++i) {
        double total = 0;
        size_t end = i;
        while (end < words.size()) {
            double amount = 0;
            size_t length = 0;
            size_t cursor = end;
            if (cursor > i) cursor += _durationJoiners.length(words, cursor);   // "an hour and 20 minutes"
            if (const auto said = _amounts.at(words, cursor)) {                  // "a", "half an", "a quarter of an"
                amount = said->first;
                cursor += said->second;
            } else if (cursor < words.size() && std::regex_match(words[cursor], _decimal)) {
                amount = std::stod(words[cursor]);
                cursor += 1;
            } else if (const auto number = smallNumber(words, cursor, length)) {
                amount = *number;
                cursor += length;
            } else {
                break;
            }
            if (const size_t half = _andAHalf.length(words, cursor)) {   // "one and a half hours"
                amount += 0.5;
                cursor += half;
            }
            const auto unit = _durationUnits.at(words, cursor);
            if (!unit) break;
            cursor += unit->second;
            if (const size_t half = _andAHalf.length(words, cursor)) {   // "an hour and a half"
                amount += 0.5;
                cursor += half;
            }
            total += amount * static_cast<double>(unit->first);
            end = cursor;
        }
        if (end == i || total <= 0) continue;
        const long seconds = std::lround(total);
        const long days = seconds / 86400, hours = seconds % 86400 / 3600, minutes = seconds % 3600 / 60, rest = seconds % 60;
        std::string value = "P";
        if (days) value += std::to_string(days) + "D";
        if (hours || minutes || rest) value += "T";
        if (hours) value += std::to_string(hours) + "H";
        if (minutes) value += std::to_string(minutes) + "M";
        if (rest) value += std::to_string(rest) + "S";
        const std::string spoken = spokenDuration(value);
        result.push_back(found(value, spoken.empty() ? value : spoken, i, end));
        i = end - 1;
    }
    return result;
}

std::vector<Found> Language::emails(const std::vector<std::string>& words) const {
    std::vector<Found> result;
    auto symbol = [this](const std::string& word) -> const std::string* {
        const auto found = _emailSymbols.find(word);
        return found == _emailSymbols.end() ? nullptr : &found->second;
    };
    for (size_t i = 0; i < words.size(); ++i) {
        if (std::regex_match(words[i], _writtenEmail)) {
            result.push_back(found(words[i], words[i], i, i + 1));
            continue;
        }
        // "john dot doe at gmail dot com": the name runs back from "at" over its symbols.
        const size_t at = _emailAt.length(words, i);
        if (!at || i == 0 || i + at + 2 > words.size()) continue;
        size_t start = i - 1;
        while (start >= 2 && symbol(words[start - 1])) start -= 2;
        std::string local;
        for (size_t word = start; word < i; ++word) local += symbol(words[word]) ? *symbol(words[word]) : words[word];
        std::string domain;
        size_t end = i + at;
        bool dotted = false;
        while (end < words.size()) {
            domain += words[end++];
            if (end + 1 < words.size() && symbol(words[end]) && *symbol(words[end]) == ".") {
                domain += '.';
                dotted = true;
                ++end;
            } else {
                break;
            }
        }
        if (!dotted || local.empty()) continue;
        const std::string value = local + "@" + domain;
        if (!std::regex_match(value, _writtenEmail)) continue;
        result.push_back(found(value, value, start, end));
        i = end - 1;
    }
    return result;
}

std::vector<Found> Language::ordinals(const std::vector<std::string>& words) const {
    std::vector<Found> result;
    for (size_t i = 0; i < words.size(); ++i) {
        size_t length = 0;
        size_t start = i;
        for (size_t back = 1; back <= std::min<size_t>(i, 2); ++back)
            if (_articles.length(words, i - back) == back) {
                start = i - back;
                break;
            }
        if (const size_t lead = _ordinalLeads.length(words, i)) {   // "number two", "option 3"
            if (const auto number = smallNumber(words, i + lead, length); number && *number >= 1) {
                result.push_back(found(std::to_string(*number), std::to_string(*number), i, i + lead + length));
                i += lead + length - 1;
            }
            continue;
        }
        if (const auto ordinal = ordinalNumber(words, i, length)) {
            size_t end = i + length;
            end += _ordinalTrailers.length(words, end);   // "the second one"
            result.push_back(found(std::to_string(*ordinal), std::to_string(*ordinal), start, end));
            i = end - 1;
        }
    }
    return result;
}

std::optional<char> Language::digit(const std::string& word) const {
    if (word.size() == 1 && std::isdigit(static_cast<unsigned char>(word[0]))) return word[0];
    const std::vector<std::string> one{word};
    if (const auto alias = _digitAliases.at(one, 0)) return static_cast<char>('0' + alias->first);
    if (const auto number = _numberWords.at(one, 0); number && number->first >= 0 && number->first < 10)
        return static_cast<char>('0' + number->first);
    return std::nullopt;
}

std::vector<Found> Language::numbers(const std::vector<std::string>& words, bool decimals) const {
    std::vector<Found> result;
    auto written = [decimals](const std::string& word) {
        bool digit = false, point = false;
        for (const char c : word) {
            if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
            else if (c == '.' && decimals && !point) point = true;
            else return false;
        }
        return digit;
    };
    for (size_t i = 0; i < words.size();) {
        if (written(words[i])) {
            const auto text = numberText(std::strtod(words[i].c_str(), nullptr));
            result.push_back(found(text, text, i, i + 1));
            ++i;
            continue;
        }
        size_t length = 0;
        const auto whole = wordNumber(words, i, length, true);
        if (!whole) {
            ++i;
            continue;
        }
        double number = static_cast<double>(*whole);
        size_t end = i + length;
        if (decimals) {   // "twenty three point five"
            if (const size_t point = _decimalPoints.length(words, end)) {
                std::string fraction;
                size_t after = end + point;
                for (; after < words.size(); ++after) {
                    const auto said = digit(words[after]);
                    if (!said) break;
                    fraction += *said;
                }
                if (!fraction.empty()) {
                    number += std::stod("0." + fraction);
                    end = after;
                }
            }
        }
        const auto text = numberText(number);
        result.push_back(found(text, text, i, end));
        i = end;
    }
    return result;
}

std::vector<Found> Language::digitRuns(const std::vector<std::string>& words, size_t fewest, size_t most) const {
    std::vector<Found> result;
    for (size_t i = 0; i < words.size();) {
        std::string digits;
        size_t j = i;
        int repeat = 1;
        for (; j < words.size(); ++j) {
            const std::vector<std::string> one{words[j]};
            if (const auto times = _repeats.at(one, 0)) {   // "double four"
                repeat = times->first;
                continue;
            }
            if (isDigits(words[j])) {
                for (int r = 0; r < repeat; ++r) digits += words[j];
            } else if (const auto said = digit(words[j])) {
                digits.append(static_cast<size_t>(repeat), *said);
            } else if (size_t length = 0; const auto pair = smallNumber(words, j, length)) {   // "douze", "thirty four"
                if (*pair < 10) break;
                for (int r = 0; r < repeat; ++r) digits += std::to_string(*pair);
                j += length - 1;
            } else {
                break;
            }
            repeat = 1;
        }
        if (digits.size() >= fewest && digits.size() <= most) result.push_back(found(digits, digits, i, j));
        i = j > i ? j : i + 1;
    }
    return result;
}

std::vector<Found> Language::yesNo(const std::vector<std::string>& words) const {
    std::vector<Found> result;
    const std::string yes = _pack.yes().empty() ? "true" : _pack.yes(0);
    const std::string no = _pack.no().empty() ? "false" : _pack.no(0);
    for (size_t i = 0; i < words.size(); ++i) {
        const auto answer = _yesNo.at(words, i);
        if (!answer) continue;
        result.push_back(found(answer->first ? "true" : "false", answer->first ? yes : no, i, i + answer->second));
        i += answer->second - 1;
    }
    return result;
}

std::optional<std::string> Language::spelledLetter(const std::string& word) const {
    if (const auto letter = _spelling.find(word); letter != _spelling.end()) return letter->second;
    if (const auto said = digit(word)) return std::string(1, *said);
    // Letters and digits written as heard ("AB 123" from the STT) are kept as they are.
    if (std::all_of(word.begin(), word.end(), [](unsigned char c) { return c < 0x80 && std::isalnum(c); })) return word;
    return std::nullopt;
}

std::string Language::spokenDate(const std::string& isoDate) const {
    int year = 0, month = 0, day = 0;
    if (!isoParts(isoDate, year, month, day) || !validDate(year, month, day)) return {};
    const auto& calendar = _pack.calendar();
    const auto& format = _pack.speaking().date();
    if (format.empty() || calendar.weekdays_size() != 7 || calendar.months_size() != 12) return {};
    const int mondayFirst = (weekdayOf(daysFromCivil(year, month, day)) + 6) % 7;
    std::string suffix;
    for (const auto& rule : _ordinalSuffixes)
        if (holds(rule, day)) {
            suffix = rule.text;
            break;
        }
    return filled(format, {{"weekday", calendar.weekdays(mondayFirst).phrases(0)},
                           {"month", calendar.months(month - 1).phrases(0)},
                           {"day_ordinal", std::to_string(day) + suffix},
                           {"day", std::to_string(day)},
                           {"year", std::to_string(year)}});
}

std::string Language::spokenTime(const std::string& time) const {
    int hour = 0, minute = 0;
    if (!clockParts(time, hour, minute)) return {};
    const auto& speaking = _pack.speaking();
    if (minute == 0 && hour == 12 && !_pack.clock().noon().empty()) return _pack.clock().noon(0);
    if (minute == 0 && hour == 0 && !_pack.clock().midnight().empty()) return _pack.clock().midnight(0);
    const std::string& format = minute == 0 && !speaking.whole_hour().empty() ? speaking.whole_hour() : speaking.time();
    if (format.empty()) return {};
    const int shown = speaking.twelve_hour() ? (hour % 12 == 0 ? 12 : hour % 12) : hour;
    char minutes[4];
    std::snprintf(minutes, sizeof(minutes), "%02d", minute);
    const std::string period = speaking.twelve_hour() ? (hour < 12 ? speaking.am() : speaking.pm()) : std::string();
    return trimmed(filled(format, {{"hour", std::to_string(shown)}, {"minute", minutes}, {"period", period}}));
}

std::string Language::spokenDateTime(const std::string& value) const {
    const auto t = value.find('T');
    if (t == std::string::npos) return {};
    const auto day = spokenDate(value.substr(0, t));
    const auto time = spokenTime(value.substr(t + 1, 5));
    if (day.empty() || time.empty()) return {};
    const auto& format = _pack.speaking().date_time();
    return filled(format.empty() ? "{date} {time}" : format, {{"date", day}, {"time", time}});
}

std::string Language::spokenDuration(const std::string& isoDuration) const {
    std::smatch parts;
    if (!std::regex_match(isoDuration, parts, _isoDuration) || isoDuration == "P" || isoDuration == "PT") return {};
    const auto& units = _pack.speaking().units();
    std::vector<std::string> pieces;
    size_t part = 0;
    for (const char* unit : {"day", "hour", "minute", "second"}) {
        ++part;
        if (!parts[part].matched) continue;
        const long amount = std::stol(parts[part].str());
        if (!amount) continue;
        const auto forms = units.find(unit);
        if (forms == units.end() || forms->second.phrases().empty()) return {};
        const std::vector<std::string> said(forms->second.phrases().begin(), forms->second.phrases().end());
        pieces.push_back(std::to_string(amount) + " " + plural(said, static_cast<double>(amount)));
    }
    return pieces.empty() ? std::string() : spokenList(pieces);
}

std::string Language::spokenList(const std::vector<std::string>& items) const {
    const auto& speaking = _pack.speaking();
    const std::string separator = speaking.list_separator().empty() ? ", " : speaking.list_separator();
    const std::string last = speaking.list_last().empty() ? separator : speaking.list_last();
    std::string text;
    for (size_t i = 0; i < items.size(); ++i) text += (i == 0 ? "" : i + 1 == items.size() ? last : separator) + items[i];
    return text;
}

std::string Language::speakable(const std::string& text) const {
    const auto& names = _pack.speaking().digits();
    auto digits = [&names](const std::string& group) {
        std::string said;
        for (const char digit : group)
            said += (said.empty() ? "" : " ") + (names.size() == 10 ? names.Get(digit - '0') : std::string(1, digit));
        return said;
    };
    std::string out;
    auto from = text.cbegin();
    std::smatch match;
    while (std::regex_search(from, text.cend(), match, _speakable)) {
        out.append(from, match[0].first);
        std::string said = match[0].str();
        if (match[1].matched) {
            if (const auto date = spokenDate(match[1].str()); !date.empty()) said = date;
        } else if (match[2].matched) {
            said = digits(match[2].str()) + ", " + digits(match[3].str()) + ", " + digits(match[4].str());
        } else if (match[5].matched) {
            const int hour = std::stoi(match[5].str());
            if (const auto time = spokenTime(clock(hour, std::stoi(match[6].str()))); !time.empty()) said = time;
        }
        out += said;
        from = match[0].second;
    }
    out.append(from, text.cend());
    return out;
}

bool Languages::negates(const std::string& word) const {
    return std::any_of(_languages.begin(), _languages.end(), [&](const auto& language) { return language->negates(word); });
}

bool Languages::isStopWord(const std::string& word) const {
    return std::any_of(_languages.begin(), _languages.end(), [&](const auto& language) { return language->isStopWord(word); });
}

std::vector<std::string> Languages::intentExamples(const std::string& intent) const {
    std::vector<std::string> examples;
    for (const auto& language : _languages) {
        const auto found = language->pack().intents().find(intent);
        if (found != language->pack().intents().end())
            examples.insert(examples.end(), found->second.phrases().begin(), found->second.phrases().end());
    }
    return examples;
}

std::vector<std::string> Languages::backchannels() const {
    std::vector<std::string> phrases;
    for (const auto& language : _languages)
        phrases.insert(phrases.end(), language->pack().turn_taking().backchannels().begin(),
                       language->pack().turn_taking().backchannels().end());
    return phrases;
}

std::vector<std::string> Languages::interruptions() const {
    std::vector<std::string> words;
    for (const auto& language : _languages)
        words.insert(words.end(), language->pack().turn_taking().interruptions().begin(),
                     language->pack().turn_taking().interruptions().end());
    return words;
}

} // namespace nlu
} // namespace va
