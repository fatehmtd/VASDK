#pragma once

#include "vasdk_export.h"
#include "Calendar.h"
#include "vasdk/nlu/language.pb.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace va {
namespace flow { class Expression; }
namespace nlu {

/**
 * Text as words are compared: lower case in the Latin, Greek and Cyrillic scripts ("Décembre" →
 * "décembre", "ÉTÉ" → "été"), and a typographic apostrophe as ' ("don’t" → "don't").
 */
VASDK_EXPORT std::string foldedText(const std::string& text);

/** A value found in what the user said. */
struct Found {
    std::string value;    // canonical: "2026-11-10", "15:30", "2026-11-03T12:00", "PT1H30M", "john.doe@gmail.com", "2"
    std::string spoken;   // how a reply says it: "Tuesday, November 10th", "3:30 PM", "1 hour and 30 minutes"
    size_t start = 0;     // the words it came from
    size_t length = 0;
};

/**
 * @brief Words of one language, each with a value: a word or a phrase of several ("day after
 *        tomorrow"), the longest that fits winning. Matched on words as flow::entityWords() splits them.
 */
template <class T>
class PhraseTable {
public:
    void add(const std::vector<std::string>& words, T value) {
        if (words.empty()) return;
        auto& candidates = _byFirst[words.front()];
        candidates.emplace_back(words, std::move(value));
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const auto& left, const auto& right) { return left.first.size() > right.first.size(); });
    }
    /** The longest phrase at words[at]: its value and how many words it takes. */
    std::optional<std::pair<T, size_t>> at(const std::vector<std::string>& words, size_t at) const {
        if (at >= words.size()) return std::nullopt;
        const auto found = _byFirst.find(words[at]);
        if (found == _byFirst.end()) return std::nullopt;
        for (const auto& [phrase, value] : found->second)
            if (at + phrase.size() <= words.size() && std::equal(phrase.begin(), phrase.end(), words.begin() + static_cast<long>(at)))
                return std::make_pair(value, phrase.size());
        return std::nullopt;
    }
    /** How many words the longest phrase at words[at] takes; 0 when none is there. */
    size_t length(const std::vector<std::string>& words, size_t at) const {
        const auto found = this->at(words, at);
        return found ? found->second : 0;
    }
    bool empty() const { return _byFirst.empty(); }

private:
    std::map<std::string, std::vector<std::pair<std::vector<std::string>, T>>> _byFirst;
};

/**
 * @brief A language as the engine reads and says it, from a vasdk.nlu.Language pack: the words for
 *        numbers, dates and times, yes and no; how values are said; the replies it says itself.
 *
 * The pack is data, written by whoever deploys the agent (vasdk/nlu/language.proto; VASDK's
 * languages/en-US.txtpb to start from). For grammar a pack can't express, subclass it and
 * override what differs, then add it with FlowRegistry::addLanguage:
 *
 * @code
 * // Dutch "half vier" is 3:30, the half hour before four: the pack's times, and these.
 * class Dutch : public va::nlu::Language {
 * public:
 *     explicit Dutch(vasdk::nlu::Language pack) : Language(std::move(pack)) {}
 *     std::vector<va::nlu::Found> times(const std::vector<std::string>& words) const override {
 *         auto found = Language::times(words);
 *         // ... "half" + smallNumber(words, i + 1, length): {"03:30", spokenTime("03:30"), i, 1 + length}
 *         return found;
 *     }
 * };
 * registry->addLanguage(std::make_shared<Dutch>(pack));
 * @endcode
 *
 * Every method reads words as flow::entityWords() splits them, and is safe to call from many
 * threads.
 */
class VASDK_EXPORT Language {
public:
    /** A pack's problems (an expression that doesn't parse, 6 weekdays) go to `errors`. */
    explicit Language(vasdk::nlu::Language pack, std::vector<std::string>* errors = nullptr);
    virtual ~Language();
    Language(const Language&) = delete;
    Language& operator=(const Language&) = delete;

    /** A pack checked: nullptr, with every problem in `error` (one per line), when it has any. */
    static std::shared_ptr<const Language> compile(vasdk::nlu::Language pack, std::string* error = nullptr);
    /** A .txtpb, .json or .pb pack file, compiled. */
    static std::shared_ptr<const Language> load(const std::string& path, std::string* error = nullptr);
    /** VASDK's own en-US pack (languages/en-US.txtpb, built into the library): what an agent that
     *  names no language speaks. A new one each call. */
    static std::shared_ptr<const Language> builtIn();
    /** The text of that pack, to copy and edit. */
    static std::string builtInText();

    const vasdk::nlu::Language& pack() const { return _pack; }
    const std::string& code() const { return _pack.code(); }
    /** Whether the pack serves `code`: the same code, or one of them is the other's language ("fr" and "fr-CA"). */
    bool serves(const std::string& code) const;

    // What the user said.
    /** "today", "next Tuesday", "the fifth", "November 10th", "in two weeks", "2026-11-10"; from `today`. */
    virtual std::vector<Found> dates(const std::vector<std::string>& words, Date today) const;
    /** "3:30 pm", "15:30", "half past three in the afternoon", "quarter to five", "noon", "at 9". */
    virtual std::vector<Found> times(const std::vector<std::string>& words) const;
    /** A date and a time in the same words: "tomorrow at noon" → "2026-11-03T12:00". */
    virtual std::vector<Found> dateTimes(const std::vector<std::string>& words, Date today) const;
    /** "an hour and a half" → "PT1H30M", "45 minutes", "half an hour", "two days". */
    virtual std::vector<Found> durations(const std::vector<std::string>& words) const;
    /** "john dot doe at gmail dot com", or written: "john.doe@gmail.com". */
    virtual std::vector<Found> emails(const std::vector<std::string>& words) const;
    /** "the second one", "2nd", "number two", "option 3" → "2". */
    virtual std::vector<Found> ordinals(const std::vector<std::string>& words) const;
    /** Numbers in digits ("25", "2.5") or words ("twenty five", "a hundred and two") → "25", "102";
     *  "five five" is two. `decimals`: "2.5" and "two point five" too. */
    virtual std::vector<Found> numbers(const std::vector<std::string>& words, bool decimals) const;
    /** Runs of digits written or said one by one ("five five five", "double four"), or in pairs
     *  ("zéro six, douze, trente quatre"), of `fewest` to `most`. */
    virtual std::vector<Found> digitRuns(const std::vector<std::string>& words, size_t fewest, size_t most) const;
    /** One-word answers: "yeah" → "true", "nope" → "false". */
    virtual std::vector<Found> yesNo(const std::vector<std::string>& words) const;
    /** A digit said: "five", "oh", "5" → '5'. */
    virtual std::optional<char> digit(const std::string& word) const;
    /** A spelled-out letter ("alpha" → "a"), or a digit; nullopt for other words. */
    virtual std::optional<std::string> spelledLetter(const std::string& word) const;
    /** A word that negates the words after it: "not". */
    bool negates(const std::string& word) const { return _negations.count(word) > 0; }
    bool isStopWord(const std::string& word) const { return _stopWords.count(word) > 0; }

    // How the reply says it.
    /** "2026-11-10" → "Tuesday, November 10th"; empty when it isn't a date. */
    virtual std::string spokenDate(const std::string& isoDate) const;
    /** "15:30" → "3:30 PM", "12:00" → "noon"; empty when it isn't a time. */
    virtual std::string spokenTime(const std::string& time) const;
    /** "2026-11-10T15:30" → "Tuesday, November 10th at 3:30 PM"; empty when it isn't one. */
    virtual std::string spokenDateTime(const std::string& value) const;
    /** "PT1H30M" → "1 hour and 30 minutes"; empty when it isn't a duration. */
    virtual std::string spokenDuration(const std::string& isoDuration) const;
    /** {"a", "b", "c"} → "a, b and c". */
    virtual std::string spokenList(const std::vector<std::string>& items) const;
    /** Which of a word's forms goes with `count`, by the pack's plural_rules: 0 is the first. */
    virtual size_t pluralForm(double count) const;
    /** One of `forms` for `count`: ["day", "days"], 3 → "days". */
    std::string plural(const std::vector<std::string>& forms, double count) const;
    /**
     * Text for a voice that doesn't read numbers itself (GenericVoiceSynthesisProvider::
     * normalizesText false): ISO dates and 24-hour times as said, phone numbers digit by digit in
     * their groups ("555-123-4567" → "five five five, one two three, four five six seven").
     */
    virtual std::string speakable(const std::string& text) const;
    /** The replies the engine says itself, in this language. */
    const vasdk::nlu::Replies& replies() const { return _pack.replies(); }

protected:
    /** 0 to 99, in digits or words: "21", "twenty one", "five"; `length`: the words it took. */
    std::optional<int> smallNumber(const std::vector<std::string>& words, size_t at, size_t& length) const;
    /** 1 to 31 as an ordinal: "fifth", "5th", "twenty first"; `length`: the words it took. */
    std::optional<int> ordinalNumber(const std::vector<std::string>& words, size_t at, size_t& length) const;

private:
    struct Rule {
        std::shared_ptr<const flow::Expression> condition;
        std::string text;
    };
    // am or pm after a time: the words it took, and the hour changed.
    size_t period(const std::vector<std::string>& words, size_t at, int& hour) const;
    // The number of words of number words at words[at] ("twenty five"); `scales`: "a hundred and two" too.
    std::optional<long long> wordNumber(const std::vector<std::string>& words, size_t at, size_t& length, bool scales) const;
    bool holds(const Rule& rule, double n) const;

    vasdk::nlu::Language _pack;
    PhraseTable<long long> _numberWords, _scales;
    PhraseTable<bool> _connectors, _ones, _decimalPoints, _ordinalLeads, _ordinalTrailers, _articles;
    PhraseTable<int> _digitAliases, _repeats, _ordinals;
    std::optional<std::regex> _ordinalSuffix;
    PhraseTable<int> _weekdays, _months, _relativeDays;   // weekdays: 0 Monday; months: 1 January
    PhraseTable<bool> _fromToday, _dayUnits, _weekUnits, _next, _coming, _dayOfMonth;
    PhraseTable<bool> _noon, _midnight, _am, _pm, _at, _hourWords, _past, _to, _zero, _minus;
    PhraseTable<int> _fractions, _afterHour;
    PhraseTable<long long> _durationUnits;
    PhraseTable<double> _amounts;
    PhraseTable<bool> _andAHalf, _durationJoiners;
    PhraseTable<bool> _emailAt;
    std::map<std::string, std::string> _emailSymbols;
    PhraseTable<bool> _yesNo;
    std::set<std::string> _negations, _stopWords;
    std::map<std::string, std::string> _spelling;
    std::vector<Rule> _ordinalSuffixes;
    std::vector<Rule> _pluralRules;
    std::regex _speakable;
    std::regex _writtenTime{R"(^([0-9]{1,2})(?:[:h]([0-9]{2}))?([a-z']*)$)"};   // "3:30", "15h30", "3pm", "15h"
    std::regex _decimal{R"(^[0-9]+(\.[0-9]+)?$)"};
    std::regex _writtenEmail{R"(^[a-z0-9._%+-]+@[a-z0-9.-]+\.[a-z]{2,}$)"};
    std::regex _isoDuration{"^P(?:([0-9]+)D)?(?:T(?:([0-9]+)H)?(?:([0-9]+)M)?(?:([0-9]+)S)?)?$"};
};

/**
 * @brief The languages an agent's users may speak, the first the one its replies are in: what it
 *        says, and how values are said. Values and the built-in intents are understood in any of them.
 */
class VASDK_EXPORT Languages {
public:
    Languages() = default;
    explicit Languages(std::vector<std::shared_ptr<const Language>> languages) : _languages(std::move(languages)) {}

    bool empty() const { return _languages.empty(); }
    /** The first language: the one replies are in. Not for an empty set. */
    const Language& speaking() const { return *_languages.front(); }
    const std::vector<std::shared_ptr<const Language>>& all() const { return _languages; }

    /** What each language finds, one after another. */
    template <class Find>
    std::vector<Found> each(Find find) const {
        std::vector<Found> found;
        for (const auto& language : _languages) {
            auto more = find(*language);
            found.insert(found.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        }
        return found;
    }
    bool negates(const std::string& word) const;
    bool isStopWord(const std::string& word) const;
    /** A built-in intent's examples in every language: sys.yes → "yes", "oui", ... */
    std::vector<std::string> intentExamples(const std::string& intent) const;
    std::vector<std::string> backchannels() const;
    std::vector<std::string> interruptions() const;

private:
    std::vector<std::shared_ptr<const Language>> _languages;
};

} // namespace nlu
} // namespace va
