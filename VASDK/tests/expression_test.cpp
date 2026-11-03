// Expressions and templates in agents: the grammar (AND/OR/NOT, comparisons, ":" membership and
// contains, lists, functions, parentheses), missing values as null, numbers in strings,
// references into lists and objects, parse errors with their column, and templates with
// $ref, ${ref}, $$ and a sentence's full stop.

#include <VASDK/flow/Expression.h>

#include <cstdio>
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

bool holds(const std::string& text) {
    std::string error;
    auto expression = Expression::parse(text, &error);
    if (!expression) {
        std::fprintf(stderr, "  parse error in %s: %s\n", text.c_str(), error.c_str());
        return false;
    }
    return expression->test(lookup);
}

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
    check(list && toText(list->evaluate(lookup)) == "a, 2 and gold" && list->references().size() == 1, "a list value");

    // Parse errors say where
    check(parseError("$session.tier = gold") == "column 17: unknown word 'gold' (strings need quotes)", "bare word");
    check(parseError("$session.tier = 'gold") == "column 17: string not closed", "unclosed string");
    check(parseError("($session.tier = 'gold'") == "column 1: expected ')'", "unclosed parenthesis");
    check(parseError("$ = 1") == "column 1: expected a name after $", "lone dollar");
    check(parseError("$session.tier = ") == "column 17: expected a value, found 'end'", "missing operand");
    check(parseError("size($session.cars)") == "column 1: unknown word 'size' (strings need quotes)", "unknown function");

    // Templates
    check(renderTemplate("You have $session.points points.", lookup) == "You have 120 points.",
          "a full stop isn't part of the reference");
    check(renderTemplate("${session.tier}-member, $$5 off, $5 fee", lookup) == "gold-member, $5 off, $5 fee",
          "${ref}, $$ and a lone $");
    check(renderTemplate("Hi $session.missing!", lookup) == "Hi !", "a missing value renders empty");
    check(renderTemplate("Your cars: $session.cars.", lookup) == "Your cars: Corolla and Civic.", "a list reads naturally");
    std::vector<Path> references;
    std::string error;
    check(templateReferences("$session.tier and ${session.points}", references, &error) && references.size() == 2,
          "template references found");
    check(!templateReferences("broken ${session.tier", references, &error) && !error.empty(), "unclosed ${ is an error");

    if (failures == 0) std::printf("expression_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
