#pragma once

#include "vasdk_export.h"

#include <google/protobuf/struct.pb.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va {
namespace nlu { class Language; }
namespace flow {

using Value = google::protobuf::Value;
/** A reference's parts: $session.cars[0] → {"session", "cars", "0"}. */
using Path = std::vector<std::string>;
/** Resolves a reference; a null Value when there's nothing there. */
using Lookup = std::function<Value(const Path& path)>;
using TimePoint = std::chrono::system_clock::time_point;

/**
 * @brief A condition or value written as a string in an agent, parsed once and evaluated per turn.
 *
 *   $session.tier : ("gold", "platinum") AND NOT empty($input.text)
 *   days_until($form.date) <= 1
 *   $session.points + 10 * len($form.services)
 *
 * Operands: references ($root.name.field, $root.list[0]), "strings" or 'strings', numbers, true,
 * false, null, lists ("a", "b"), function calls and parentheses. Operators, tightest first:
 * - (negation); * / %; + -; = != < <= > >= and ":" (in a list, or a string contains another,
 * ignoring case); then NOT, AND, OR (keywords in any case). A missing value is null; a string that
 * reads as a number compares and adds as one. In + and -, null is 0; + joins text when either side
 * isn't a number. Division by zero is null.
 *
 * Functions:
 *   exists(x) empty(x) len(x) lower(x) upper(x) number(x)
 *   round(x) round(x, digits) floor(x) ceil(x) abs(x) min(a, b, ...) max(a, b, ...)
 *   now()               the local date and time: "2026-11-05T14:30:00"
 *   today()             the local date: "2026-11-05"
 *   days_until(date)    whole days from today to a date (or the date of a date and time); null when it isn't one
 *   days_since(date)    the same, the other way
 *   add_days(date, n)   "2026-11-05", 3 → "2026-11-08"
 *   weekday(date)       1 Monday ... 7 Sunday
 * Dates are ISO text, so they also compare in order: $form.date > today().
 */
class VASDK_EXPORT Expression {
public:
    struct Node;

    /** nullptr, with "column 12: expected ')'" in `error`, when `text` doesn't parse. */
    static std::shared_ptr<const Expression> parse(const std::string& text, std::string* error = nullptr);

    /** `now` is what now() and today() read, and days count from; unset: the clock. */
    Value evaluate(const Lookup& lookup, std::optional<TimePoint> now = std::nullopt) const;
    bool test(const Lookup& lookup, std::optional<TimePoint> now = std::nullopt) const;
    /** Every reference in it, for checking them when the agent loads. */
    const std::vector<Path>& references() const { return _references; }
    /** When the whole expression is one reference ($session.tier): its path; else nullptr. */
    const Path* reference() const;

private:
    std::shared_ptr<const Node> _root;
    std::vector<Path> _references;
};

/**
 * What a reply's template reads: a value as a reply says it ("Tuesday, November 10th") and as it
 * is ("2026-11-10").
 */
struct TemplateScope {
    Lookup spoken;
    Lookup raw;                      // empty: `spoken`
    std::optional<TimePoint> now;    // for now(), today() and days_until(); unset: the clock
    const nlu::Language* language = nullptr;   // the reply's: lists ("a, b and c"), |plural; nullptr: "a, b, c", 1 or more
};

/**
 * Replaces the references in a reply: "You have $session.points points.", ${session.points}, or
 * an expression in braces: ${$session.points * 2}, ${days_until($form.date)}. "$$" is a "$". A
 * missing value renders as "". Lists render as the language says them: "a, b and c".
 *
 * Filters after "|", in order: ${form.date|raw} (the value, not as said: "2026-11-10"), |count (a
 * list's items), |plural:"car":"cars" ("1 car", "3 cars"; a list counts its items; the forms the
 * language's plural_rules pick, 2 to 6 of them), |round or
 * |round:2, |lower, |upper, |capitalize, |default:"no one". A reference is said, unless a filter
 * needs the value (raw, count, plural, round); an expression reads the values.
 */
VASDK_EXPORT std::string renderTemplate(const std::string& text, const TemplateScope& scope);
VASDK_EXPORT std::string renderTemplate(const std::string& text, const Lookup& lookup);
/** The references in a template; false, with `error` set, for an unclosed ${, an expression
 *  that doesn't parse or an unknown filter. */
VASDK_EXPORT bool templateReferences(const std::string& text, std::vector<Path>& references,
                                     std::string* error = nullptr);

/** null, false, 0, "" and empty lists or objects are false. */
VASDK_EXPORT bool isTruthy(const Value& value);
/** A value as text: 3 not 3.0, true/false, "" for null, a list as "a, b, c". */
VASDK_EXPORT std::string toText(const Value& value);
VASDK_EXPORT bool valuesEqual(const Value& left, const Value& right);
/** Walks `path` from `path[from]` into objects (by name) and lists (by index); null when absent. */
VASDK_EXPORT Value valueAt(const Value& root, const Path& path, size_t from);
/** "$session.tier" from {"session", "tier"}. */
VASDK_EXPORT std::string pathText(const Path& path);

VASDK_EXPORT Value nullValue();
VASDK_EXPORT Value boolValue(bool value);
VASDK_EXPORT Value numberValue(double value);
VASDK_EXPORT Value stringValue(const std::string& value);

} // namespace flow
} // namespace va
