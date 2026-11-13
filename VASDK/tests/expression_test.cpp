// Expressions and templates in agents: the grammar (AND/OR/NOT, comparisons, ":" membership and
// contains, lists, functions, parentheses), missing values as null, numbers in strings,
// references into lists and objects, parse errors with their column, and templates with
// $ref, ${ref}, $$ and a sentence's full stop. Arithmetic, dates against a fixed clock, and
// templates with expressions and filters.

#include <VASDK/flow/Expression.h>
#include <VASDK/nlu/Language.h>

#include <gtest/gtest.h>

#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace va::flow;

namespace {

class ExpressionTest : public testing::Test {
protected:
    // $session.<name>; $session.cars is a list, $session.car an object.
    ExpressionTest() {
        _session = {{"tier", stringValue("gold")}, {"points", numberValue(120)}, {"verified", boolValue(true)},
                    {"zip", stringValue("02134")}, {"count", stringValue("7")}};
        Value cars;
        *cars.mutable_list_value()->add_values() = stringValue("Corolla");
        *cars.mutable_list_value()->add_values() = stringValue("Civic");
        _session["cars"] = cars;
        Value car;
        (*car.mutable_struct_value()->mutable_fields())["make"] = stringValue("Toyota");
        _session["car"] = car;
    }

    const Lookup lookup = [this](const Path& path) {
        if (path.size() < 2 || path[0] != "session") return nullValue();
        const auto found = _session.find(path[1]);
        return found == _session.end() ? nullValue() : valueAt(found->second, path, 2);
    };

    // 2026-11-05 (a Thursday), noon, local time.
    static TimePoint thursdayNoon() {
        std::tm local{};
        local.tm_year = 2026 - 1900;
        local.tm_mon = 10;
        local.tm_mday = 5;
        local.tm_hour = 12;
        local.tm_isdst = -1;
        return std::chrono::system_clock::from_time_t(std::mktime(&local));
    }

    bool holds(const std::string& text) const {
        std::string error;
        auto expression = Expression::parse(text, &error);
        if (!expression) {
            ADD_FAILURE() << "parse error in " << text << ": " << error;
            return false;
        }
        return expression->test(lookup, thursdayNoon());
    }

    Value valueOf(const std::string& text) const {
        std::string error;
        auto expression = Expression::parse(text, &error);
        if (!expression) {
            ADD_FAILURE() << "parse error in " << text << ": " << error;
            return nullValue();
        }
        return expression->evaluate(lookup, thursdayNoon());
    }

    std::string textOf(const std::string& text) const { return toText(valueOf(text)); }

    static std::string parseError(const std::string& text) {
        std::string error;
        return Expression::parse(text, &error) ? "" : error;
    }

private:
    std::map<std::string, Value> _session;
};

TEST_F(ExpressionTest, ComparesAndCombines) {
    EXPECT_TRUE(holds("$session.tier = 'gold'") && holds("$session.tier = \"gold\"") && holds("$session.tier != 'silver'"))
        << "= and != with either quote";
    EXPECT_TRUE(holds("$session.points > 100 AND $session.points <= 120")) << "numbers compare";
    EXPECT_TRUE(holds("$session.count = 7") && holds("$session.count < 10")) << "a numeric string compares as a number";
    EXPECT_TRUE(holds("$session.zip = '02134'")) << "a string stays a string against a string";
    EXPECT_TRUE(holds("$session.verified") && holds("$session.verified = true")) << "booleans";
    EXPECT_TRUE(holds("$session.tier = 'silver' OR $session.points > 50")) << "OR";
    EXPECT_TRUE(holds("NOT $session.tier = 'silver'") && holds("not ($session.tier = 'gold' and false)"))
        << "NOT, keywords in any case, parentheses";
    EXPECT_TRUE(holds("$session.points > 100 AND $session.tier = 'gold' OR false")) << "AND binds tighter than OR";
}

TEST_F(ExpressionTest, TestsMembershipAndContains) {
    EXPECT_TRUE(holds("$session.tier : ('gold', 'platinum')")) << "in a list";
    EXPECT_FALSE(holds("$session.tier : ('silver', 'bronze')")) << "not in a list";
    EXPECT_TRUE(holds("$session.cars : 'Civic'")) << "a list contains a value";
    EXPECT_TRUE(holds("'I need an Oil Change' : 'oil change'")) << "a string contains another, ignoring case";
}

TEST_F(ExpressionTest, TreatsMissingValuesAsNull) {
    EXPECT_TRUE(holds("$session.missing = null") && holds("NOT exists($session.missing)")) << "a missing value is null";
    EXPECT_FALSE(holds("$session.missing = 'gold'")) << "null compares false";
    EXPECT_FALSE(holds("$session.missing > 3")) << "null compares false";
    EXPECT_TRUE(holds("empty($session.missing)")) << "empty()";
    EXPECT_FALSE(holds("empty($session.tier)")) << "empty()";
}

TEST_F(ExpressionTest, ReadsListsObjectsAndFunctions) {
    EXPECT_TRUE(holds("$session.cars[1] = 'Civic'") && holds("$session.cars[5] = null")) << "list index";
    EXPECT_TRUE(holds("$session.car.make = 'Toyota'")) << "object field";
    EXPECT_TRUE(holds("len($session.cars) = 2") && holds("len($session.tier) = 4")) << "len()";
    EXPECT_TRUE(holds("upper($session.tier) = 'GOLD'") && holds("lower('ABC') = 'abc'")) << "upper() and lower()";
}

TEST_F(ExpressionTest, GivesAValueAndWhatItRefersTo) {
    auto value = Expression::parse("$session.points");
    ASSERT_NE(value, nullptr);
    ASSERT_TRUE(value->reference()) << "a lone reference";
    EXPECT_EQ(*value->reference(), Path({"session", "points"})) << "a lone reference: its path";
    EXPECT_EQ(value->evaluate(lookup).number_value(), 120) << "a lone reference: its value";
    auto list = Expression::parse("('a', 2, $session.tier)");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(toText(list->evaluate(lookup)), "a, 2, gold") << "a list value";
    EXPECT_EQ(list->references().size(), 1u) << "a list value's references";
}

TEST_F(ExpressionTest, SaysWhereAParseErrorIs) {
    EXPECT_EQ(parseError("$session.tier = gold"), "column 17: unknown word 'gold' (strings need quotes)") << "bare word";
    EXPECT_EQ(parseError("$session.tier = 'gold"), "column 17: string not closed") << "unclosed string";
    EXPECT_EQ(parseError("($session.tier = 'gold'"), "column 1: expected ')'") << "unclosed parenthesis";
    EXPECT_EQ(parseError("$ = 1"), "column 1: expected a name after $") << "lone dollar";
    EXPECT_EQ(parseError("$session.tier = "), "column 17: expected a value, found 'end'") << "missing operand";
    EXPECT_EQ(parseError("size($session.cars)"), "column 1: unknown word 'size' (strings need quotes)") << "unknown function";
}

TEST_F(ExpressionTest, DoesArithmetic) {
    EXPECT_EQ(textOf("$session.points + 10 * 2"), "140") << "* binds tighter than +";
    EXPECT_EQ(textOf("($session.points + 10) * 2"), "260") << "parentheses first";
    EXPECT_EQ(textOf("$session.points - 20"), "100") << "minus with spaces";
    EXPECT_EQ(textOf("$session.points-20"), "100") << "minus without spaces";
    EXPECT_EQ(textOf("-$session.points"), "-120") << "negation";
    EXPECT_EQ(textOf("$session.count * 2"), "14") << "a numeric string";
    EXPECT_EQ(textOf("7 / 2"), "3.5") << "/";
    EXPECT_EQ(textOf("7 % 3"), "1") << "%";
    EXPECT_EQ(valueOf("1 / 0").kind_case(), Value::kNullValue) << "division by zero is null";
    EXPECT_EQ(valueOf("'a' * 2").kind_case(), Value::kNullValue) << "text times a number is null";
    EXPECT_EQ(textOf("$session.missing + 1"), "1") << "a missing value adds as 0";
    EXPECT_EQ(textOf("'Hi ' + $session.tier"), "Hi gold") << "+ joins text";
    EXPECT_EQ(textOf("0.1 + 0.2"), "0.3") << "0.1 + 0.2 reads as 0.3";
    EXPECT_TRUE(holds("$session.points * 2 > 200") && holds("$session.points + 1 = 121")) << "arithmetic in a condition";
    EXPECT_EQ(textOf("('a', -5)"), "a, -5") << "a negative number in a list";
    EXPECT_EQ(textOf("round(2.567, 2)"), "2.57") << "round";
    EXPECT_EQ(textOf("round(2.5)"), "3") << "round";
    EXPECT_EQ(textOf("floor(2.9)"), "2") << "floor";
    EXPECT_EQ(textOf("ceil(2.1)"), "3") << "ceil";
    EXPECT_EQ(textOf("abs(-4)"), "4") << "abs";
    EXPECT_EQ(textOf("min(3, $session.points, 7)"), "3") << "min";
    EXPECT_EQ(textOf("max(3, $session.points)"), "120") << "max";
    EXPECT_EQ(textOf("number('42') + 1"), "43") << "number";
}

TEST_F(ExpressionTest, ReadsDatesAgainstTheClockGiven) {
    EXPECT_EQ(textOf("today()"), "2026-11-05") << "today()";
    EXPECT_EQ(textOf("now()"), "2026-11-05T12:00:00") << "now()";
    EXPECT_EQ(textOf("days_until('2026-11-10')"), "5") << "days_until";
    EXPECT_EQ(textOf("days_until('2026-11-10T08:00')"), "5") << "days_until a time";
    EXPECT_EQ(textOf("days_since('2026-11-01')"), "4") << "days_since";
    EXPECT_EQ(textOf("days_until(today())"), "0") << "days_until today";
    EXPECT_TRUE(holds("days_until('2026-11-06') <= 1")) << "a booking for tomorrow";
    EXPECT_FALSE(holds("days_until('2026-11-09') <= 1")) << "a booking for next week";
    EXPECT_EQ(valueOf("days_until('next week')").kind_case(), Value::kNullValue) << "not a date: null";
    EXPECT_EQ(valueOf("days_until('2026-02-30')").kind_case(), Value::kNullValue) << "not a date: null";
    EXPECT_EQ(textOf("add_days('2026-11-05', 30)"), "2026-12-05") << "add_days";
    EXPECT_EQ(textOf("add_days(today(), -5)"), "2026-10-31") << "add_days back";
    EXPECT_EQ(textOf("weekday('2026-11-05')"), "4") << "weekday: Thursday";
    EXPECT_EQ(textOf("weekday('2026-11-08')"), "7") << "weekday: 7 Sunday";
    EXPECT_EQ(textOf("weekday('2026-11-09')"), "1") << "weekday: 1 Monday";
    EXPECT_TRUE(holds("'2026-11-10' > today()") && holds("weekday(today()) <= 5")) << "ISO dates compare in order";
    EXPECT_EQ(parseError("today(1)"), "column 1: today takes 0 values") << "a function's values are counted";
    EXPECT_EQ(parseError("add_days(today())"), "column 1: add_days takes 2 values") << "a function's values are counted";
    EXPECT_EQ(parseError("round()"), "column 1: round takes 1 or 2 values") << "a function's values are counted";
}

TEST_F(ExpressionTest, RendersTemplates) {
    EXPECT_EQ(renderTemplate("You have $session.points points.", lookup), "You have 120 points.")
        << "a full stop isn't part of the reference";
    EXPECT_EQ(renderTemplate("${session.tier}-member, $$5 off, $5 fee", lookup), "gold-member, $5 off, $5 fee")
        << "${ref}, $$ and a lone $";
    EXPECT_EQ(renderTemplate("Hi $session.missing!", lookup), "Hi !") << "a missing value renders empty";
    EXPECT_EQ(renderTemplate("Your cars: $session.cars.", lookup), "Your cars: Corolla, Civic.") << "a list without a language";

    const auto english = va::nlu::Language::builtIn();
    const TemplateScope scope{lookup, nullptr, std::nullopt, english.get()};
    EXPECT_EQ(renderTemplate("Your cars: $session.cars.", scope), "Your cars: Corolla and Civic.")
        << "a list reads in the reply's language";
    EXPECT_EQ(renderTemplate("${session.cars|plural:'car':'cars'}, ${1|plural:'car':'cars'}", scope), "2 cars, 1 car")
        << "plural by the language's rules";

    std::vector<Path> references;
    std::string error;
    EXPECT_TRUE(templateReferences("$session.tier and ${session.points}", references, &error)) << error;
    EXPECT_EQ(references.size(), 2u) << "template references found";
    EXPECT_FALSE(templateReferences("broken ${session.tier", references, &error)) << "unclosed ${ is an error";
    EXPECT_FALSE(error.empty()) << "unclosed ${ says why";
}

// "spoken" says a value as the reply would (a date, a tier's name); raw is the value.
TEST_F(ExpressionTest, RendersExpressionsAndFiltersInTemplates) {
    TemplateScope scope;
    scope.raw = lookup;
    scope.spoken = [this](const Path& path) { return path.size() == 2 && path[1] == "tier" ? stringValue("Gold member") : lookup(path); };
    scope.now = thursdayNoon();
    EXPECT_EQ(renderTemplate("You are $session.tier (${session.tier|raw}).", scope), "You are Gold member (gold).")
        << "|raw: the value, not as said";
    EXPECT_EQ(renderTemplate("${$session.points * 2} points, in ${days_until('2026-11-10')} days.", scope),
              "240 points, in 5 days.") << "an expression in braces";
    EXPECT_EQ(renderTemplate("${session.cars|count} cars; ${session.cars|plural:\"car\":\"cars\"}; ${1|plural:'car':'cars'}", scope),
              "2 cars; 2 cars; 1 car") << "|count and |plural";
    EXPECT_EQ(renderTemplate("${session.missing|default:\"no one\"} and ${session.tier|raw|upper} ${'tea'|capitalize}", scope),
              "no one and GOLD Tea") << "|default, filters in order, |capitalize";
    EXPECT_EQ(renderTemplate("${$session.points / 7|round:1}", scope), "17.1") << "|round:1";
    EXPECT_EQ(renderTemplate("${'a|b'|upper}", scope), "A|B") << "a | inside quotes isn't a filter";

    std::vector<Path> references;
    std::string error;
    EXPECT_TRUE(templateReferences("${$session.points + $session.count|round} ${session.tier|raw}", references, &error)) << error;
    EXPECT_EQ(references.size(), 3u) << "references in expressions and filtered ones";
    EXPECT_FALSE(templateReferences("${session.tier|shout}", references, &error));
    EXPECT_NE(error.find("unknown filter 'shout'"), std::string::npos) << "an unknown filter is an error: " << error;
    EXPECT_FALSE(templateReferences("${session.tier|plural:'car'}", references, &error));
    EXPECT_NE(error.find("takes 2 to 6 values"), std::string::npos) << "a filter's values are counted: " << error;
    EXPECT_FALSE(templateReferences("${$session.points +}", references, &error));
    EXPECT_NE(error.find("expected a value"), std::string::npos) << "an expression that doesn't parse is an error: " << error;
}

} // namespace
