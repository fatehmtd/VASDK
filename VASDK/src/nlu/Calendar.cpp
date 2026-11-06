#include "nlu/Calendar.h"

#include <cstdio>
#include <ctime>

namespace va {
namespace nlu {

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

} // namespace nlu
} // namespace va
