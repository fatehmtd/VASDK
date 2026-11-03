#pragma once

#include "vasdk_export.h"

#include <google/protobuf/struct.pb.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace va {
namespace flow {

using Value = google::protobuf::Value;
/** A reference's parts: $session.cars[0] → {"session", "cars", "0"}. */
using Path = std::vector<std::string>;
/** Resolves a reference; a null Value when there's nothing there. */
using Lookup = std::function<Value(const Path& path)>;

/**
 * @brief A condition or value written as a string in an agent, parsed once and evaluated per turn.
 *
 *   $session.tier : ("gold", "platinum") AND NOT empty($input.text)
 *
 * Operands: references ($root.name.field, $root.list[0]), "strings" or 'strings', numbers, true,
 * false, null, lists ("a", "b"), calls exists(x) empty(x) len(x) lower(x) upper(x), and
 * parentheses. Operators: = != < <= > >= and ":" (in a list, or a string contains another,
 * ignoring case), then NOT, AND, OR (keywords in any case). A missing value is null; a string that
 * reads as a number compares as one.
 */
class VASDK_EXPORT Expression {
public:
    struct Node;

    /** nullptr, with "column 12: expected ')'" in `error`, when `text` doesn't parse. */
    static std::shared_ptr<const Expression> parse(const std::string& text, std::string* error = nullptr);

    Value evaluate(const Lookup& lookup) const;
    bool test(const Lookup& lookup) const;
    /** Every reference in it, for checking them when the agent loads. */
    const std::vector<Path>& references() const { return _references; }
    /** When the whole expression is one reference ($session.tier): its path; else nullptr. */
    const Path* reference() const;

private:
    std::shared_ptr<const Node> _root;
    std::vector<Path> _references;
};

/**
 * Replaces the references in a reply: "You have $session.points points." or ${session.points};
 * "$$" is a "$". A missing value renders as "". Lists render as "a, b and c".
 */
VASDK_EXPORT std::string renderTemplate(const std::string& text, const Lookup& lookup);
/** The references in a template; false, with `error` set, for an unclosed ${. */
VASDK_EXPORT bool templateReferences(const std::string& text, std::vector<Path>& references,
                                     std::string* error = nullptr);

/** null, false, 0, "" and empty lists or objects are false. */
VASDK_EXPORT bool isTruthy(const Value& value);
/** How a value reads in a reply: 3 not 3.0, true/false, "" for null. */
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
