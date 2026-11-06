#pragma once

#include "vasdk_export.h"

#include <chrono>
#include <string>

namespace va {
namespace nlu {

/** A day of the proleptic Gregorian calendar, whatever the language. */
struct Date {
    int year = 0, month = 0, day = 0;
};
/** The local date at `now`. */
VASDK_EXPORT Date dateAt(std::chrono::system_clock::time_point now);
/** Days since 1970-01-01, and back. */
VASDK_EXPORT long daysFromCivil(int year, int month, int day);
VASDK_EXPORT long daysOf(Date date);
VASDK_EXPORT Date civilFromDays(long days);
/** 0: Sunday ... 6: Saturday. */
VASDK_EXPORT int weekdayOf(long days);
VASDK_EXPORT Date addDays(Date date, long days);
VASDK_EXPORT bool validDate(int year, int month, int day);
/** "2026-11-10". */
VASDK_EXPORT std::string iso(Date date);

} // namespace nlu
} // namespace va
