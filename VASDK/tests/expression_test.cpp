// Expressions and templates in agents: the grammar (AND/OR/NOT, comparisons, ":" membership and
// contains, lists, functions, parentheses), missing values as null, numbers in strings,
// references into lists and objects, parse errors with their column, and templates with
// $ref, ${ref}, $$ and a sentence's full stop. Arithmetic, dates against a fixed clock, and
// templates with expressions and filters.

#include <VASDK/flow/Expression.h>
#include <VASDK/nlu/Language.h>

#include <cstdio>
#include <ctime>
#include <map>
#include <string>

using namespace va::flow;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

// $session.<name> from a map; $session.cars is a list, $session.car an object.
Value lookup(const Path& path) {
    static const std::map<std::string, Value> session = [] {
        std::map<std::string, Value> values{{"tier", stringValue("gold")}, {"points", numberValue(120)},
                                            {"verified", boolValue(true)}, {"zip", stringValue("02134")},
                                            {"count", stringValue("7")}};
        Value cars;
        *cars.mutable_list_value()->add_values() = stringValue("Corolla");
        *cars.mutable_list_value()->add_values() = stringValue("Civic");
        values["cars"] = cars;
        Value car;
        (*car.mutable_struct_value()->mutable_fields())["make"] = stringValue("Toyota");
        values["car"] = car;
        return values;
    }();
    if (path.size() < 2 || path[0] != "session") return nullValue();
    const auto found = session.find(path[1]);
    return found == session.end() ? nullValue() : valueAt(found->second, path, 2);
}

// 2026-11-05 (a Thursday), noon, local time.
TimePoint thursdayNoon() {
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 10;
    local.tm_mday = 5;
    local.tm_hour = 12;
    local.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

bool holds(const std::string& text) {
    std::string error;
    auto expression = Expression::parse(text, &error);
    if (!expression) {
        std::fprintf(stderr, "  parse error in %s: %s\n", text.c_str(), error.c_str());
        return false;
    }
    return expression->test(lookup, thursdayNoon());
}

Value valueOf(const std::string& text) {
    std::string error;
    auto expression = Expression::parse(text, &error);
    if (!expression) {
        std::fprintf(stderr, "  parse error in %s: %s\n", text.c_str(), error.c_str());
        return nullValue();
    }
    return expression->evaluate(lookup, thursdayNoon());
}
std::string textOf(const std::string& text) { return toText(valueOf(text)); }

std::string parseError(const std::string& text) {
    std::string error;
    return Expression::parse(text, &error) ? "" : error;
}

} // namespace

int main() {
    // Comparisons and logic
    check(holds("$session.tier = 'gold'") && holds("$session.tier = \"gold\"") && holds("$session.tier != 'silver'"),
          "= and != with either quote");
    check(holds("$session.points > 100 AND $session.points <= 120"), "numbers compare");
    check(holds("$session.count = 7") && holds("$session.count < 10"), "a numeric string compares as a number");
    check(holds("$session.zip = '02134'"), "a string stays a string against a string");
    check(holds("$session.verified") && holds("$session.verified = true"), "booleans");
    check(holds("$session.tier = 'silver' OR $session.points > 50"), "OR");
    check(holds("NOT $session.tier = 'silver'") && holds("not ($session.tier = 'gold' and false)"),
          "NOT, keywords in any case, parentheses");
    check(holds("$session.points > 100 AND $session.tier = 'gold' OR false"), "AND binds tighter than OR");

    // Membership and contains
    check(holds("$session.tier : ('gold', 'platinum')") && !holds("$session.tier : ('silver', 'bronze')"),
          "in a list");
    check(holds("$session.cars : 'Civic'"), "a list contains a value");
    check(holds("'I need an Oil Change' : 'oil change'"), "a string contains another, ignoring case");

    // Missing values
    check(holds("$session.missing = null") && holds("NOT exists($session.missing)"), "a missing value is null");
    check(!holds("$session.missing = 'gold'") && !holds("$session.missing > 3"), "null compares false");
    check(holds("empty($session.missing)") && !holds("empty($session.tier)"), "empty()");

    // References into lists and objects; functions
    check(holds("$session.cars[1] = 'Civic'") && holds("$session.cars[5] = null"), "list index");
    check(holds("$session.car.make = 'Toyota'"), "object field");
    check(holds("len($session.cars) = 2") && holds("len($session.tier) = 4"), "len()");
    check(holds("upper($session.tier) = 'GOLD'") && holds("lower('ABC') = 'abc'"), "upper() and lower()");

    // The value of an expression, and what it refers to
    auto value = Expression::parse("$session.points");
    check(value && value->reference() && *value->reference() == Path({"session", "points"}) &&
              value->evaluate(lookup).number_value() == 120, "a lone reference: its path and value");
    auto list = Expression::parse("('a', 2, $session.tier)");
    check(list && toText(list->evaluate(lookup)) == "a, 2, gold" && list->references().size() == 1, "a list value");

    // Parse errors say where
    check(parseError("$session.tier = gold") == "column 17: unknown word 'gold' (strings need quotes)", "bare word");
    check(parseError("$session.tier = 'gold") == "column 17: string not closed", "unclosed string");
    check(parseError("($session.tier = 'gold'") == "column 1: expected ')'", "unclosed parenthesis");
    check(parseError("$ = 1") == "column 1: expected a name after $", "lone dollar");
    check(parseError("$session.tier = ") == "column 17: expected a value, found 'end'", "missing operand");
    check(parseError("size($session.cars)") == "column 1: unknown word 'size' (strings need quotes)", "unknown function");

    // Arithmetic
    check(textOf("$session.points + 10 * 2") == "140" && textOf("($session.points + 10) * 2") == "260",
          "* binds tighter than +, parentheses first");
    check(textOf("$session.points - 20") == "100" && textOf("$session.points-20") == "100" && textOf("-$session.points") == "-120",
          "minus, with or without spaces, and negation");
    check(textOf("$session.count * 2") == "14" && textOf("7 / 2") == "3.5" && textOf("7 % 3") == "1", "/ and %, a numeric string");
    check(valueOf("1 / 0").kind_case() == Value::kNullValue && valueOf("'a' * 2").kind_case() == Value::kNullValue,
          "division by zero and text times a number are null");
    check(textOf("$session.missing + 1") == "1", "a missing value adds as 0");
    check(textOf("'Hi ' + $session.tier") == "Hi gold" && textOf("0.1 + 0.2") == "0.3", "+ joins text; 0.1 + 0.2 reads as 0.3");
    check(holds("$session.points * 2 > 200") && holds("$session.points + 1 = 121"), "arithmetic in a condition");
    check(textOf("('a', -5)") == "a, -5", "a negative number in a list");
    check(textOf("round(2.567, 2)") == "2.57" && textOf("round(2.5)") == "3" && textOf("floor(2.9)") == "2" &&
              textOf("ceil(2.1)") == "3" && textOf("abs(-4)") == "4", "round, floor, ceil, abs");
    check(textOf("min(3, $session.points, 7)") == "3" && textOf("max(3, $session.points)") == "120" &&
              textOf("number('42') + 1") == "43", "min, max, number");

    // Dates, against the clock given
    check(textOf("today()") == "2026-11-05" && textOf("now()") == "2026-11-05T12:00:00", "today() and now()");
    check(textOf("days_until('2026-11-10')") == "5" && textOf("days_until('2026-11-10T08:00')") == "5" &&
              textOf("days_since('2026-11-01')") == "4" && textOf("days_until(today())") == "0", "days_until and days_since");
    check(holds("days_until('2026-11-06') <= 1") && !holds("days_until('2026-11-09') <= 1"), "a booking for tomorrow");
    check(valueOf("days_until('next week')").kind_case() == Value::kNullValue && valueOf("days_until('2026-02-30')").kind_case() == Value::kNullValue,
          "not a date: null");
    check(textOf("add_days('2026-11-05', 30)") == "2026-12-05" && textOf("add_days(today(), -5)") == "2026-10-31", "add_days");
    check(textOf("weekday('2026-11-05')") == "4" && textOf("weekday('2026-11-08')") == "7" && textOf("weekday('2026-11-09')") == "1",
          "weekday: 1 Monday ... 7 Sunday");
    check(holds("'2026-11-10' > today()") && holds("weekday(today()) <= 5"), "ISO dates compare in order");
    check(parseError("today(1)") == "column 1: today takes 0 values" && parseError("add_days(today())") == "column 1: add_days takes 2 values" &&
              parseError("round()") == "column 1: round takes 1 or 2 values", "a function's values are counted");

    // Templates
    check(renderTemplate("You have $session.points points.", lookup) == "You have 120 points.",
          "a full stop isn't part of the reference");
    check(renderTemplate("${session.tier}-member, $$5 off, $5 fee", lookup) == "gold-member, $5 off, $5 fee",
          "${ref}, $$ and a lone $");
    check(renderTemplate("Hi $session.missing!", lookup) == "Hi !", "a missing value renders empty");
    check(renderTemplate("Your cars: $session.cars.", lookup) == "Your cars: Corolla, Civic.", "a list without a language");
    {
        const auto english = va::nlu::Language::builtIn();
        const TemplateScope scope{lookup, nullptr, std::nullopt, english.get()};
        check(renderTemplate("Your cars: $session.cars.", scope) == "Your cars: Corolla and Civic.", "a list reads in the reply's language");
        check(renderTemplate("${session.cars|plural:'car':'cars'}, ${1|plural:'car':'cars'}", scope) == "2 cars, 1 car",
              "plural by the language's rules");
    }
    std::vector<Path> references;
    std::string error;
    check(templateReferences("$session.tier and ${session.points}", references, &error) && references.size() == 2,
          "template references found");
    check(!templateReferences("broken ${session.tier", references, &error) && !error.empty(), "unclosed ${ is an error");

    // Templates: expressions and filters. "spoken" says a date; raw is the value.
    TemplateScope scope;
    scope.raw = lookup;
    scope.spoken = [](const Path& path) { return path.size() == 2 && path[1] == "tier" ? stringValue("Gold member") : lookup(path); };
    scope.now = thursdayNoon();
    check(renderTemplate("You are $session.tier (${session.tier|raw}).", scope) == "You are Gold member (gold).", "|raw: the value, not as said");
    check(renderTemplate("${$session.points * 2} points, in ${days_until('2026-11-10')} days.", scope) == "240 points, in 5 days.",
          "an expression in braces");
    check(renderTemplate("${session.cars|count} cars; ${session.cars|plural:\"car\":\"cars\"}; ${1|plural:'car':'cars'}", scope) ==
              "2 cars; 2 cars; 1 car", "|count and |plural");
    check(renderTemplate("${session.missing|default:\"no one\"} and ${session.tier|raw|upper} ${'tea'|capitalize}", scope) ==
              "no one and GOLD Tea", "|default, filters in order, |capitalize");
    check(renderTemplate("${$session.points / 7|round:1}", scope) == "17.1", "|round:1");
    check(renderTemplate("${'a|b'|upper}", scope) == "A|B", "a | inside quotes isn't a filter");
    references.clear();
    check(templateReferences("${$session.points + $session.count|round} ${session.tier|raw}", references, &error) && references.size() == 3,
          "references in expressions and filtered ones");
    check(!templateReferences("${session.tier|shout}", references, &error) && error.find("unknown filter 'shout'") != std::string::npos,
          "an unknown filter is an error");
    check(!templateReferences("${session.tier|plural:'car'}", references, &error) && error.find("takes 2 to 6 values") != std::string::npos,
          "a filter's values are counted");
    check(!templateReferences("${$session.points +}", references, &error) && error.find("expected a value") != std::string::npos,
          "an expression that doesn't parse is an error");

    if (failures == 0) std::printf("expression_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
