#include "nlu/English.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <map>
#include <regex>

namespace va {
namespace nlu {
namespace english {

namespace {

const std::vector<std::string> kMonths{"january", "february", "march", "april", "may", "june", "july",
                                       "august", "september", "october", "november", "december"};
const std::vector<std::string> kMonthsShort{"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
const std::vector<std::string> kWeekdays{"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
const std::vector<std::string> kUnits{"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                      "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
                                      "eighteen", "nineteen"};
const std::vector<std::string> kTens{"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
const std::vector<std::string> kOrdinals{"", "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth",
                                         "ninth", "tenth", "eleventh", "twelfth", "thirteenth", "fourteenth", "fifteenth",
                                         "sixteenth", "seventeenth", "eighteenth", "nineteenth"};

bool isDigits(const std::string& word) {
    return !word.empty() && std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c); });
}

int indexOf(const std::vector<std::string>& list, const std::string& word) {
    const auto found = std::find(list.begin(), list.end(), word);
    return found == list.end() ? -1 : static_cast<int>(found - list.begin());
}

// 0 to 99, in digits or words: "21", "twenty one", "five". `length`: the words it took.
std::optional<int> smallNumber(const std::vector<std::string>& words, size_t i, size_t& length) {
    if (i >= words.size()) return std::nullopt;
    if (isDigits(words[i]) && words[i].size() <= 2) {
        length = 1;
        return std::stoi(words[i]);
    }
    if (const int unit = indexOf(kUnits, words[i]); unit >= 0) {
        length = 1;
        return unit;
    }
    if (const int ten = indexOf(kTens, words[i]); ten >= 2) {
        if (i + 1 < words.size()) {
            const int unit = indexOf(kUnits, words[i + 1]);
            if (unit >= 1 && unit <= 9) {
                length = 2;
                return ten * 10 + unit;
            }
        }
        length = 1;
        return ten * 10;
    }
    return std::nullopt;
}

// 1 to 31 as an ordinal: "fifth", "5th", "twenty first", "thirtieth".
std::optional<int> ordinalNumber(const std::vector<std::string>& words, size_t i, size_t& length) {
    if (i >= words.size()) return std::nullopt;
    static const std::regex suffixed("^([0-9]{1,2})(st|nd|rd|th)$");
    std::smatch digits;
    if (std::regex_match(words[i], digits, suffixed)) {
        length = 1;
        return std::stoi(digits[1].str());
    }
    if (const int ordinal = indexOf(kOrdinals, words[i]); ordinal >= 1) {
        length = 1;
        return ordinal;
    }
    if (words[i] == "twentieth" || words[i] == "thirtieth") {
        length = 1;
        return words[i] == "twentieth" ? 20 : 30;
    }
    if ((words[i] == "twenty" || words[i] == "thirty") && i + 1 < words.size()) {
        const int unit = indexOf(kOrdinals, words[i + 1]);
        if (unit >= 1 && unit <= 9) {
            length = 2;
            return (words[i] == "twenty" ? 20 : 30) + unit;
        }
    }
    return std::nullopt;
}

// Days since 1970-01-01 and back (Howard Hinnant's civil calendar algorithms).
long daysFromCivil(int year, int month, int day) {
    year -= month <= 2;
    const long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<long>(dayOfEra) - 719468;
}

Date civilFromDays(long days) {
    days += 719468;
    const long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPart = (5 * dayOfYear + 2) / 153;
    Date date;
    date.day = static_cast<int>(dayOfYear - (153 * monthPart + 2) / 5 + 1);
    date.month = static_cast<int>(monthPart < 10 ? monthPart + 3 : monthPart - 9);
    date.year = static_cast<int>(yearOfEra) + static_cast<int>(era) * 400 + (date.month <= 2);
    return date;
}

long daysOf(Date date) { return daysFromCivil(date.year, date.month, date.day); }
int weekdayOf(long days) { return static_cast<int>((days % 7 + 11) % 7); }   // 0: Sunday (1970-01-01 was a Thursday)
Date addDays(Date date, long days) { return civilFromDays(daysOf(date) + days); }

bool validDate(int year, int month, int day) {
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    return civilFromDays(daysFromCivil(year, month, day)).day == day;
}

std::string iso(Date date) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", date.year, date.month, date.day);
    return text;
}

std::string clock(int hour, int minute) {
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
    return text;
}

int monthAt(const std::vector<std::string>& words, size_t i) {
    if (i >= words.size()) return -1;
    if (const int month = indexOf(kMonths, words[i]); month >= 0) return month + 1;
    if (words[i] == "sept") return 9;
    if (const int month = indexOf(kMonthsShort, words[i]); month >= 0) return month + 1;
    return -1;
}

// A month and day without a year: the next one from today.
Date upcoming(Date today, int month, int day) {
    Date date{today.year, month, day};
    if (daysOf(date) < daysOf(today)) date.year += 1;
    return date;
}

Found found(std::string value, std::string spoken, size_t start, size_t end) {
    Found result;
    result.value = std::move(value);
    result.spoken = std::move(spoken);
    result.start = start;
    result.length = end - start;
    return result;
}

std::string ordinalSuffix(int day) {
    if (day % 100 >= 11 && day % 100 <= 13) return "th";
    switch (day % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
    }
}

// am/pm or a part of the day after a time: the words it took and how the hour changes.
size_t period(const std::vector<std::string>& words, size_t i, int& hour) {
    auto afternoon = [&] { if (hour < 12) hour += 12; };
    auto morning = [&] { if (hour == 12) hour = 0; };
    if (i < words.size() && (words[i] == "pm" || words[i] == "am")) {
        words[i] == "pm" ? afternoon() : morning();
        return 1;
    }
    if (i + 1 < words.size() && (words[i] == "p" || words[i] == "a") && words[i + 1] == "m") {
        words[i] == "p" ? afternoon() : morning();
        return 2;
    }
    if (i + 2 < words.size() && words[i] == "in" && words[i + 1] == "the") {
        if (words[i + 2] == "afternoon" || words[i + 2] == "evening") {
            afternoon();
            return 3;
        }
        if (words[i + 2] == "morning") {
            morning();
            return 3;
        }
    }
    if (i + 1 < words.size() && words[i] == "at" && words[i + 1] == "night") {
        afternoon();
        return 2;
    }
    if (i < words.size() && words[i] == "tonight") {
        afternoon();
        return 1;
    }
    return 0;
}

} // namespace

Date dateAt(std::chrono::system_clock::time_point now) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
}

std::vector<Found> dates(const std::vector<std::string>& words, Date today) {
    std::vector<Found> result;
    auto add = [&](Date date, size_t start, size_t end) { result.push_back(found(iso(date), spokenDate(iso(date)), start, end)); };
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& word = words[i];
        size_t length = 0;
        // 2026-11-10
        if (word.size() == 4 && isDigits(word) && i + 2 < words.size() && isDigits(words[i + 1]) && isDigits(words[i + 2]) &&
            validDate(std::stoi(word), std::stoi(words[i + 1]), std::stoi(words[i + 2]))) {
            add({std::stoi(word), std::stoi(words[i + 1]), std::stoi(words[i + 2])}, i, i + 3);
            i += 2;
            continue;
        }
        if (word == "today" || word == "tonight") { add(today, i, i + 1); continue; }
        if (word == "tomorrow") { add(addDays(today, 1), i, i + 1); continue; }
        if (word == "yesterday") { add(addDays(today, -1), i, i + 1); continue; }
        if (word == "day" && i + 2 < words.size() && words[i + 1] == "after" && words[i + 2] == "tomorrow") {
            add(addDays(today, 2), i, i + 3);
            i += 2;
            continue;
        }
        // "in two weeks", "in a week", "in 3 days"
        if (word == "in" && i + 2 < words.size()) {
            std::optional<int> count = words[i + 1] == "a" ? std::optional<int>(1) : smallNumber(words, i + 1, length);
            if (words[i + 1] == "a") length = 1;
            const size_t unitAt = i + 1 + length;
            if (count && unitAt < words.size()) {
                const auto& unit = words[unitAt];
                const int days = unit == "day" || unit == "days" ? 1 : unit == "week" || unit == "weeks" ? 7 : 0;
                if (days) {
                    add(addDays(today, *count * days), i, unitAt + 1);
                    i = unitAt;
                    continue;
                }
            }
        }
        if (word == "next" && i + 1 < words.size() && words[i + 1] == "week") {
            add(addDays(today, 7), i, i + 2);
            ++i;
            continue;
        }
        // "next Tuesday": the Tuesday of next week; "Tuesday", "this Tuesday": the next one from today
        const bool next = word == "next";
        const size_t dayAt = next || word == "this" || word == "on" ? i + 1 : i;
        if (const int weekday = dayAt < words.size() ? indexOf(kWeekdays, words[dayAt]) : -1; weekday >= 0) {
            const long todayDays = daysOf(today);
            long ahead = (weekday - weekdayOf(todayDays) + 7) % 7;
            if (ahead == 0) ahead = 7;
            if (next) {
                const long weekStart = todayDays - (weekdayOf(todayDays) + 6) % 7;   // Monday
                if (todayDays + ahead < weekStart + 7) ahead += 7;                   // still this week: the one after
            }
            add(addDays(today, ahead), i, dayAt + 1);
            i = dayAt;
            continue;
        }
        // "November 10th", "nov 10", "November tenth 2027"
        if (const int month = monthAt(words, i); month > 0) {
            std::optional<int> day = ordinalNumber(words, i + 1, length);
            if (!day) day = smallNumber(words, i + 1, length);
            if (day && validDate(today.year, month, *day)) {
                size_t end = i + 1 + length;
                Date date = upcoming(today, month, *day);
                if (end < words.size() && words[end].size() == 4 && isDigits(words[end])) date.year = std::stoi(words[end++]);
                add(date, i, end);
                i = end - 1;
                continue;
            }
        }
        // "the fifth", "the 10th of November", "10 November"
        const size_t ordinalAt = word == "the" || (word == "on" && i + 1 < words.size() && words[i + 1] == "the")
                                     ? (word == "on" ? i + 2 : i + 1) : i;
        std::optional<int> day = ordinalNumber(words, ordinalAt, length);
        bool spelledOrdinal = day.has_value();
        if (!day && ordinalAt == i) day = smallNumber(words, i, length);
        if (day && *day >= 1 && *day <= 31) {
            size_t end = ordinalAt + length;
            const size_t monthWord = end < words.size() && words[end] == "of" ? end + 1 : end;
            if (const int month = monthAt(words, monthWord); month > 0 && validDate(today.year, month, *day)) {
                add(upcoming(today, month, *day), i, monthWord + 1);
                i = monthWord;
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

std::vector<Found> times(const std::vector<std::string>& words) {
    std::vector<Found> result;
    static const std::regex written("^([0-9]{1,2}):([0-9]{2})(am|pm)?$");
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& word = words[i];
        const size_t start = i > 0 && words[i - 1] == "at" ? i - 1 : i;
        auto add = [&](int hour, int minute, size_t end) {
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
            result.push_back(found(clock(hour, minute), spokenTime(clock(hour, minute)), start, end));
            i = end - 1;
            return true;
        };
        if (word == "noon" || word == "midday") { add(12, 0, i + 1); continue; }
        if (word == "midnight") { add(0, 0, i + 1); continue; }
        std::smatch parts;
        if (std::regex_match(word, parts, written)) {   // "3:30", "15:30", "3:30pm"
            int hour = std::stoi(parts[1].str());
            if (parts[3].matched) {
                if (parts[3].str() == "pm" && hour < 12) hour += 12;
                if (parts[3].str() == "am" && hour == 12) hour = 0;
                add(hour, std::stoi(parts[2].str()), i + 1);
            } else {
                const size_t taken = period(words, i + 1, hour);
                add(hour, std::stoi(parts[2].str()), i + 1 + taken);
            }
            continue;
        }
        // "half past three", "quarter to five", "twenty past six"
        size_t length = 0;
        std::optional<int> minutes = word == "half" ? std::optional<int>(30) : word == "quarter" ? std::optional<int>(15)
                                                                                                 : smallNumber(words, i, length);
        if (word == "half" || word == "quarter") length = 1;
        if (minutes && i + length + 1 < words.size() && (words[i + length] == "past" || words[i + length] == "to")) {
            size_t hourLength = 0;
            if (auto hour = smallNumber(words, i + length + 1, hourLength); hour && *hour >= 1 && *hour <= 12) {
                int h = *hour, m = *minutes;
                if (words[i + length] == "to") {
                    h = h == 1 ? 12 : h - 1;
                    m = 60 - m;
                }
                const size_t end = i + length + 1 + hourLength;
                const size_t taken = period(words, end, h);
                add(h, m, end + taken);
                continue;
            }
        }
        // "three thirty pm", "9 am", "three o'clock", "at 9"
        size_t hourLength = 0;
        const auto hour = smallNumber(words, i, hourLength);
        if (!hour || *hour > 23) continue;
        size_t end = i + hourLength;
        int minute = 0;
        bool shaped = start < i;   // "at 9"
        if (end < words.size() && (words[end] == "o'clock" || words[end] == "oclock")) {
            ++end;
            shaped = true;
        } else if (end < words.size() && words[end] == "oh" && end + 1 < words.size()) {   // "nine oh five"
            size_t minuteLength = 0;
            if (const auto m = smallNumber(words, end + 1, minuteLength); m && *m < 10) {
                minute = *m;
                end += 1 + minuteLength;
                shaped = true;
            }
        } else if (size_t minuteLength = 0; end < words.size()) {
            if (const auto m = smallNumber(words, end, minuteLength); m && *m >= 10 && *m < 60) {
                minute = *m;
                end += minuteLength;
                shaped = true;
            }
        }
        int h = *hour;
        const size_t taken = period(words, end, h);
        if (taken) shaped = true;
        if (!shaped || (taken == 0 && *hour > 12 && minute == 0 && start == i)) continue;
        add(h, minute, end + taken);
    }
    return result;
}

std::vector<Found> dateTimes(const std::vector<std::string>& words, Date today) {
    const auto onDays = dates(words, today);
    const auto atTimes = times(words);
    if (onDays.empty() || atTimes.empty()) return {};
    const auto& date = onDays.front();
    const auto& time = atTimes.front();
    const size_t start = std::min(date.start, time.start);
    const size_t end = std::max(date.start + date.length, time.start + time.length);
    return {found(date.value + "T" + time.value, date.spoken + " at " + time.spoken, start, end)};
}

std::vector<Found> durations(const std::vector<std::string>& words) {
    std::vector<Found> result;
    static const std::regex decimal("^[0-9]+(\\.[0-9]+)?$");
    auto unitSeconds = [](const std::string& unit) -> long {
        if (unit == "second" || unit == "seconds" || unit == "sec" || unit == "secs") return 1;
        if (unit == "minute" || unit == "minutes" || unit == "min" || unit == "mins") return 60;
        if (unit == "hour" || unit == "hours" || unit == "hr" || unit == "hrs") return 3600;
        if (unit == "day" || unit == "days") return 86400;
        if (unit == "week" || unit == "weeks") return 604800;
        return 0;
    };
    for (size_t i = 0; i < words.size(); ++i) {
        double total = 0;
        size_t end = i;
        while (end < words.size()) {
            double amount = 0;
            size_t length = 0;
            size_t cursor = end;
            if (words[cursor] == "and" && cursor > i) ++cursor;
            if (cursor + 2 < words.size() && words[cursor] == "half" && (words[cursor + 1] == "an" || words[cursor + 1] == "a")) {
                amount = 0.5;   // "half an hour"
                cursor += 2;
            } else if (cursor + 3 < words.size() && words[cursor] == "a" && words[cursor + 1] == "quarter" && words[cursor + 2] == "of") {
                amount = 0.25;   // "a quarter of an hour"
                cursor += 4;
            } else if (cursor < words.size() && (words[cursor] == "a" || words[cursor] == "an")) {
                amount = 1;
                cursor += 1;
            } else if (cursor < words.size() && std::regex_match(words[cursor], decimal)) {
                amount = std::stod(words[cursor]);
                cursor += 1;
            } else if (const auto number = smallNumber(words, cursor, length)) {
                amount = *number;
                cursor += length;
            } else {
                break;
            }
            // "one and a half hours"
            if (cursor + 2 < words.size() && words[cursor] == "and" && words[cursor + 1] == "a" && words[cursor + 2] == "half") {
                amount += 0.5;
                cursor += 3;
            }
            const long seconds = cursor < words.size() ? unitSeconds(words[cursor]) : 0;
            if (!seconds) break;
            ++cursor;
            // "an hour and a half"
            if (cursor + 2 < words.size() && words[cursor] == "and" && words[cursor + 1] == "a" && words[cursor + 2] == "half") {
                amount += 0.5;
                cursor += 3;
            }
            total += amount * static_cast<double>(seconds);
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
        result.push_back(found(value, spokenDuration(value), i, end));
        i = end - 1;
    }
    return result;
}

std::vector<Found> emails(const std::vector<std::string>& words) {
    std::vector<Found> result;
    static const std::regex written("^[a-z0-9._%+-]+@[a-z0-9.-]+\\.[a-z]{2,}$");
    static const std::map<std::string, char> joiners{{"dot", '.'}, {"underscore", '_'}, {"dash", '-'}, {"hyphen", '-'}};
    for (size_t i = 0; i < words.size(); ++i) {
        if (std::regex_match(words[i], written)) {
            result.push_back(found(words[i], words[i], i, i + 1));
            continue;
        }
        // "john dot doe at gmail dot com": the name runs back from "at" over its joiners.
        if (words[i] != "at" || i == 0 || i + 3 > words.size()) continue;
        size_t start = i - 1;
        while (start >= 2 && joiners.count(words[start - 1])) start -= 2;
        std::string local;
        for (size_t at = start; at < i; ++at) local += joiners.count(words[at]) ? std::string(1, joiners.at(words[at])) : words[at];
        std::string domain;
        size_t end = i + 1;
        bool dotted = false;
        while (end < words.size()) {
            domain += words[end++];
            if (end + 1 < words.size() && words[end] == "dot") {
                domain += '.';
                dotted = true;
                ++end;
            } else {
                break;
            }
        }
        if (!dotted || local.empty()) continue;
        const std::string value = local + "@" + domain;
        if (!std::regex_match(value, written)) continue;
        result.push_back(found(value, value, start, end));
        i = end - 1;
    }
    return result;
}

std::vector<Found> ordinals(const std::vector<std::string>& words) {
    std::vector<Found> result;
    for (size_t i = 0; i < words.size(); ++i) {
        size_t length = 0;
        const size_t start = i > 0 && words[i - 1] == "the" ? i - 1 : i;
        if ((words[i] == "number" || words[i] == "option") && i + 1 < words.size()) {   // "number two", "option 3"
            if (const auto number = smallNumber(words, i + 1, length); number && *number >= 1) {
                result.push_back(found(std::to_string(*number), std::to_string(*number), i, i + 1 + length));
                i += length;
            }
            continue;
        }
        if (const auto ordinal = ordinalNumber(words, i, length)) {
            size_t end = i + length;
            if (end < words.size() && words[end] == "one") ++end;   // "the second one"
            result.push_back(found(std::to_string(*ordinal), std::to_string(*ordinal), start, end));
            i = end - 1;
        }
    }
    return result;
}

std::string spokenDate(const std::string& isoDate) {
    static const std::regex shape("^([0-9]{4})-([0-9]{2})-([0-9]{2})$");
    std::smatch parts;
    if (!std::regex_match(isoDate, parts, shape)) return {};
    const int year = std::stoi(parts[1].str()), month = std::stoi(parts[2].str()), day = std::stoi(parts[3].str());
    if (!validDate(year, month, day)) return {};
    std::string weekday = kWeekdays[static_cast<size_t>(weekdayOf(daysFromCivil(year, month, day)))];
    std::string monthName = kMonths[static_cast<size_t>(month - 1)];
    weekday[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(weekday[0])));
    monthName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(monthName[0])));
    return weekday + ", " + monthName + " " + std::to_string(day) + ordinalSuffix(day);
}

std::string spokenTime(const std::string& time) {
    static const std::regex shape("^([0-9]{2}):([0-9]{2})$");
    std::smatch parts;
    if (!std::regex_match(time, parts, shape)) return {};
    const int hour = std::stoi(parts[1].str()), minute = std::stoi(parts[2].str());
    if (hour > 23 || minute > 59) return {};
    if (minute == 0 && hour == 12) return "noon";
    if (minute == 0 && hour == 0) return "midnight";
    const int twelve = hour % 12 == 0 ? 12 : hour % 12;
    char text[16];
    if (minute == 0) std::snprintf(text, sizeof(text), "%d %s", twelve, hour < 12 ? "AM" : "PM");
    else std::snprintf(text, sizeof(text), "%d:%02d %s", twelve, minute, hour < 12 ? "AM" : "PM");
    return text;
}

std::string spokenDuration(const std::string& isoDuration) {
    static const std::regex shape("^P(?:([0-9]+)D)?(?:T(?:([0-9]+)H)?(?:([0-9]+)M)?(?:([0-9]+)S)?)?$");
    std::smatch parts;
    if (!std::regex_match(isoDuration, parts, shape) || isoDuration == "P" || isoDuration == "PT") return {};
    const char* names[] = {"day", "hour", "minute", "second"};
    std::vector<std::string> pieces;
    for (int i = 0; i < 4; ++i) {
        if (!parts[static_cast<size_t>(i + 1)].matched) continue;
        const long amount = std::stol(parts[static_cast<size_t>(i + 1)].str());
        if (amount) pieces.push_back(std::to_string(amount) + " " + names[i] + (amount == 1 ? "" : "s"));
    }
    if (pieces.empty()) return {};
    std::string text = pieces[0];
    for (size_t i = 1; i < pieces.size(); ++i) text += (i + 1 == pieces.size() ? " and " : ", ") + pieces[i];
    return text;
}

std::string speakable(const std::string& text) {
    static const std::regex value(
        "\\b([0-9]{4}-[0-9]{2}-[0-9]{2})\\b"                                    // a date
        "|\\(?\\b([0-9]{3})\\)?[-. ]([0-9]{3})[-. ]([0-9]{4})\\b"               // a phone number
        "|\\b([01]?[0-9]|2[0-3]):([0-5][0-9])\\b(?!\\s*[AaPp]\\.?[Mm])");      // a 24-hour time
    auto digits = [](const std::string& group) {
        std::string said;
        for (const char digit : group) said += (said.empty() ? "" : " ") + kUnits[static_cast<size_t>(digit - '0')];
        return said;
    };
    std::string out;
    auto from = text.cbegin();
    std::smatch match;
    while (std::regex_search(from, text.cend(), match, value)) {
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

} // namespace english
} // namespace nlu
} // namespace va
