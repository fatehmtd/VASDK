#pragma once

#include "vasdk_export.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace nlu {
namespace english {

/**
 * English values in what the user said, from either speech-recognition style: words ("half past
 * three in the afternoon", "john dot doe at gmail dot com") or digits ("3:30 pm"). They work on
 * words as flow::entityWords() splits them; dates count from `today`.
 */
struct Found {
    std::string value;    // canonical: "2026-11-10", "15:30", "2026-11-03T12:00", "PT1H30M", "john.doe@gmail.com", "2"
    std::string spoken;   // how a reply says it: "Tuesday, November 10th", "3:30 PM", "an hour and a half"
    size_t start = 0;     // the words it came from
    size_t length = 0;
};

struct Date {
    int year = 0, month = 0, day = 0;
};
/** The local date at `now`. */
VASDK_EXPORT Date dateAt(std::chrono::system_clock::time_point now);

/** "today", "tomorrow", "next Tuesday" (the Tuesday of next week), "the fifth", "November 10th", "in two weeks", "2026-11-10". */
VASDK_EXPORT std::vector<Found> dates(const std::vector<std::string>& words, Date today);
/** "3:30 pm", "15:30", "half past three in the afternoon", "quarter to five", "noon", "at 9". */
VASDK_EXPORT std::vector<Found> times(const std::vector<std::string>& words);
/** A date and a time in the same words: "tomorrow at noon" → "2026-11-03T12:00". */
VASDK_EXPORT std::vector<Found> dateTimes(const std::vector<std::string>& words, Date today);
/** "an hour and a half" → "PT1H30M", "45 minutes", "half an hour", "two days". */
VASDK_EXPORT std::vector<Found> durations(const std::vector<std::string>& words);
/** "john dot doe at gmail dot com", or written: "john.doe@gmail.com". */
VASDK_EXPORT std::vector<Found> emails(const std::vector<std::string>& words);
/** "the second one", "2nd", "number two", "option 3" → "2". */
VASDK_EXPORT std::vector<Found> ordinals(const std::vector<std::string>& words);

/** "2026-11-10" → "Tuesday, November 10th"; empty when it isn't a date. */
VASDK_EXPORT std::string spokenDate(const std::string& isoDate);
/** "15:30" → "3:30 PM", "12:00" → "noon"; empty when it isn't a time. */
VASDK_EXPORT std::string spokenTime(const std::string& time);
/** "PT1H30M" → "1 hour and 30 minutes"; empty when it isn't a duration. */
VASDK_EXPORT std::string spokenDuration(const std::string& isoDuration);
/**
 * Text for a voice that doesn't read numbers well itself (GenericVoiceSynthesisProvider::
 * normalizesText false): ISO dates and 24-hour times as said, phone numbers digit by digit in
 * their groups ("555-123-4567" → "five five five, one two three, four five six seven").
 */
VASDK_EXPORT std::string speakable(const std::string& text);

} // namespace english
} // namespace nlu
} // namespace va
