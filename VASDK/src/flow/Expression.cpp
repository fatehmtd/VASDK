#include "flow/Expression.h"

#include "nlu/English.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <stdexcept>

namespace va {
namespace flow {

Value nullValue() {
    Value value;
    value.set_null_value(google::protobuf::NULL_VALUE);
    return value;
}
Value boolValue(bool flag) {
    Value value;
    value.set_bool_value(flag);
    return value;
}
Value numberValue(double number) {
    Value value;
    value.set_number_value(number);
    return value;
}
Value stringValue(const std::string& text) {
    Value value;
    value.set_string_value(text);
    return value;
}

namespace {

bool isNull(const Value& value) {
    return value.kind_case() == Value::KIND_NOT_SET || value.kind_case() == Value::kNullValue;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string trimmed(const std::string& text) {
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

// A number, or a string that reads entirely as one ("42", " 3.5 ").
std::optional<double> asNumber(const Value& value) {
    if (value.kind_case() == Value::kNumberValue) return value.number_value();
    if (value.kind_case() != Value::kStringValue) return std::nullopt;
    const std::string& text = value.string_value();
    const char* start = text.c_str();
    char* end = nullptr;
    const double number = std::strtod(start, &end);
    if (end == start) return std::nullopt;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end) return std::nullopt;
    return number;
}

// The date of "2026-11-10" or "2026-11-10T08:00", as days since 1970-01-01.
std::optional<long> asDays(const Value& value) {
    if (value.kind_case() != Value::kStringValue) return std::nullopt;
    const std::string& text = value.string_value();
    int year = 0, month = 0, day = 0;
    if (text.size() < 10 || text[4] != '-' || text[7] != '-' || (text.size() > 10 && text[10] != 'T' && text[10] != ' '))
        return std::nullopt;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2d", &year, &month, &day) != 3 || !nlu::english::validDate(year, month, day))
        return std::nullopt;
    return nlu::english::daysFromCivil(year, month, day);
}

std::string localDateTime(TimePoint now) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[24];
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return text;
}

bool identifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identifierPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Reads a reference's path from text[position] (just after "$"). Returns where it ends, or
// npos when no name starts there. A "." only continues it when a name follows, so the full
// stop in "You have $session.points." isn't taken.
size_t readPath(const std::string& text, size_t position, Path& path) {
    auto readName = [&](size_t at, std::string& name) {
        if (at >= text.size() || !identifierStart(text[at])) return std::string::npos;
        size_t end = at;
        while (end < text.size() && identifierPart(text[end])) ++end;
        name = text.substr(at, end - at);
        return end;
    };
    std::string name;
    size_t end = readName(position, name);
    if (end == std::string::npos) return std::string::npos;
    path = {name};
    while (end < text.size()) {
        if (text[end] == '.' && end + 1 < text.size() && identifierStart(text[end + 1])) {
            end = readName(end + 1, name);
            path.push_back(name);
        } else if (text[end] == '[') {
            size_t close = end + 1;
            while (close < text.size() && std::isdigit(static_cast<unsigned char>(text[close]))) ++close;
            if (close == end + 1 || close >= text.size() || text[close] != ']') break;
            path.push_back(text.substr(end + 1, close - end - 1));
            end = close + 1;
        } else {
            break;
        }
    }
    return end;
}

struct ParseError {
    size_t column;   // 1-based
    std::string message;
};

struct Token {
    enum class Type { Reference, String, Number, Word, Operator, OpenParenthesis, CloseParenthesis, Comma, End };
    Type type;
    std::string text;
    Path path;
    double number = 0;
    size_t column = 0;
};

// The operators, longest first so "<=" isn't read as "<".
const char* operatorAt(const std::string& text, size_t i) {
    const char* operators[] = {"!=", "<=", ">=", "==", "=", "<", ">", ":", "+", "-", "*", "/", "%"};
    for (const char* op : operators)
        if (text.compare(i, std::char_traits<char>::length(op), op) == 0) return op;
    return nullptr;
}

std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (true) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        Token token;
        token.column = i + 1;
        if (i >= text.size()) {
            token.type = Token::Type::End;
            tokens.push_back(token);
            return tokens;
        }
        const char c = text[i];
        if (c == '$') {
            const size_t end = readPath(text, i + 1, token.path);
            if (end == std::string::npos) throw ParseError{i + 1, "expected a name after $"};
            token.type = Token::Type::Reference;
            i = end;
        } else if (c == '"' || c == '\'') {
            size_t j = i + 1;
            for (; j < text.size() && text[j] != c; ++j) {
                if (text[j] == '\\' && j + 1 < text.size()) ++j;
                token.text += text[j];
            }
            if (j >= text.size()) throw ParseError{i + 1, "string not closed"};
            token.type = Token::Type::String;
            i = j + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            char* end = nullptr;
            token.number = std::strtod(text.c_str() + i, &end);
            token.type = Token::Type::Number;
            i = static_cast<size_t>(end - text.c_str());
        } else if (identifierStart(c)) {
            size_t j = i;
            while (j < text.size() && identifierPart(text[j])) ++j;
            token.type = Token::Type::Word;
            token.text = text.substr(i, j - i);
            i = j;
        } else if (c == '(' || c == ')' || c == ',') {
            token.type = c == '(' ? Token::Type::OpenParenthesis
                       : c == ')' ? Token::Type::CloseParenthesis : Token::Type::Comma;
            ++i;
        } else if (const char* op = operatorAt(text, i)) {
            token.type = Token::Type::Operator;
            token.text = std::string(op) == "==" ? "=" : op;
            i += std::char_traits<char>::length(op);
        } else {
            throw ParseError{i + 1, std::string("unexpected '") + c + "'"};
        }
        tokens.push_back(token);
    }
}

bool isComparison(const std::string& op) {
    return op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" || op == ":";
}

// How many values a function takes: {fewest, most}; nullopt for a name that isn't one.
std::optional<std::pair<int, int>> arity(const std::string& name) {
    if (name == "now" || name == "today") return std::pair{0, 0};
    if (name == "exists" || name == "empty" || name == "len" || name == "lower" || name == "upper" || name == "number" ||
        name == "floor" || name == "ceil" || name == "abs" || name == "days_until" || name == "days_since" ||
        name == "weekday")
        return std::pair{1, 1};
    if (name == "round") return std::pair{1, 2};
    if (name == "add_days") return std::pair{2, 2};
    if (name == "min" || name == "max") return std::pair{1, 64};
    return std::nullopt;
}

} // namespace

struct Expression::Node {
    enum class Kind { Literal, Reference, List, Not, And, Or, Compare, Arithmetic, Negate, Call };
    Kind kind;
    Value literal;
    Path path;
    std::string name;   // the operator or function
    std::vector<std::shared_ptr<const Node>> children;
};

namespace {

using Node = Expression::Node;
using NodePointer = std::shared_ptr<const Node>;

class Parser {
public:
    Parser(std::vector<Token> tokens, std::vector<Path>& references)
        : _tokens(std::move(tokens)), _references(references) {}

    NodePointer parseAll() {
        auto node = parseOr();
        if (peek().type != Token::Type::End) throw ParseError{peek().column, "unexpected '" + describe(peek()) + "'"};
        return node;
    }

private:
    const Token& peek() const { return _tokens[_position]; }
    const Token& take() { return _tokens[_position++]; }
    bool takeWord(const char* word) {
        if (peek().type == Token::Type::Word && lowered(peek().text) == word) {
            ++_position;
            return true;
        }
        return false;
    }
    bool takeOperator(const char* first, const char* second, std::string& op) {
        if (peek().type != Token::Type::Operator || (peek().text != first && peek().text != second)) return false;
        op = take().text;
        return true;
    }
    static std::string describe(const Token& token) {
        switch (token.type) {
        case Token::Type::End: return "end";
        case Token::Type::OpenParenthesis: return "(";
        case Token::Type::CloseParenthesis: return ")";
        case Token::Type::Comma: return ",";
        case Token::Type::Reference: return pathText(token.path);
        default: return token.text;
        }
    }
    static NodePointer make(Node::Kind kind, std::string name, std::vector<NodePointer> children) {
        auto node = std::make_shared<Node>();
        node->kind = kind;
        node->name = std::move(name);
        node->children = std::move(children);
        return node;
    }

    NodePointer parseOr() {
        auto left = parseAnd();
        while (takeWord("or")) left = make(Node::Kind::Or, "OR", {left, parseAnd()});
        return left;
    }
    NodePointer parseAnd() {
        auto left = parseNot();
        while (takeWord("and")) left = make(Node::Kind::And, "AND", {left, parseNot()});
        return left;
    }
    NodePointer parseNot() {
        if (takeWord("not")) return make(Node::Kind::Not, "NOT", {parseNot()});
        return parseCompare();
    }
    NodePointer parseCompare() {
        auto left = parseSum();
        if (peek().type != Token::Type::Operator || !isComparison(peek().text)) return left;
        const std::string op = take().text;
        return make(Node::Kind::Compare, op, {left, parseSum()});
    }
    NodePointer parseSum() {
        auto left = parseProduct();
        for (std::string op; takeOperator("+", "-", op);) left = make(Node::Kind::Arithmetic, op, {left, parseProduct()});
        return left;
    }
    NodePointer parseProduct() {
        auto left = parseUnary();
        for (std::string op; takeOperator("*", "/", op) || takeOperator("%", "%", op);)
            left = make(Node::Kind::Arithmetic, op, {left, parseUnary()});
        return left;
    }
    NodePointer parseUnary() {
        if (std::string op; takeOperator("-", "-", op)) {
            auto operand = parseUnary();
            if (operand->kind == Node::Kind::Literal && operand->literal.kind_case() == Value::kNumberValue) {
                auto negative = std::make_shared<Node>(*operand);   // -5 stays a literal: ("a", -5) is a list of values
                negative->literal = numberValue(-operand->literal.number_value());
                return negative;
            }
            return make(Node::Kind::Negate, "-", {operand});
        }
        return parseOperand();
    }
    NodePointer parseOperand() {
        const Token& token = take();
        auto node = std::make_shared<Node>();
        switch (token.type) {
        case Token::Type::Reference:
            node->kind = Node::Kind::Reference;
            node->path = token.path;
            _references.push_back(token.path);
            return node;
        case Token::Type::String:
            node->kind = Node::Kind::Literal;
            node->literal = stringValue(token.text);
            return node;
        case Token::Type::Number:
            node->kind = Node::Kind::Literal;
            node->literal = numberValue(token.number);
            return node;
        case Token::Type::Word: {
            const std::string word = lowered(token.text);
            node->kind = Node::Kind::Literal;
            if (word == "true" || word == "false") {
                node->literal = boolValue(word == "true");
                return node;
            }
            if (word == "null") {
                node->literal = nullValue();
                return node;
            }
            const auto takes = arity(word);
            if (!takes) throw ParseError{token.column, "unknown word '" + token.text + "' (strings need quotes)"};
            if (take().type != Token::Type::OpenParenthesis) throw ParseError{token.column, "expected '(' after " + word};
            std::vector<NodePointer> arguments;
            if (peek().type != Token::Type::CloseParenthesis) {
                arguments.push_back(parseOr());
                while (peek().type == Token::Type::Comma) {
                    take();
                    arguments.push_back(parseOr());
                }
            }
            if (take().type != Token::Type::CloseParenthesis) throw ParseError{token.column, "expected ')' after " + word + "'s values"};
            const int count = static_cast<int>(arguments.size());
            if (count < takes->first || count > takes->second) {
                const std::string wanted = takes->first == takes->second ? std::to_string(takes->first)
                                         : takes->second > 2 ? "at least " + std::to_string(takes->first)
                                                             : std::to_string(takes->first) + " or " + std::to_string(takes->second);
                throw ParseError{token.column, word + " takes " + wanted + (wanted == "1" ? " value" : " values")};
            }
            return make(Node::Kind::Call, word, std::move(arguments));
        }
        case Token::Type::OpenParenthesis: {
            auto first = parseOr();
            if (peek().type != Token::Type::Comma) {
                if (take().type != Token::Type::CloseParenthesis) throw ParseError{token.column, "expected ')'"};
                return first;
            }
            std::vector<NodePointer> items{first};
            while (peek().type == Token::Type::Comma) {
                take();
                items.push_back(parseOr());
            }
            if (take().type != Token::Type::CloseParenthesis) throw ParseError{token.column, "expected ')' to close the list"};
            return make(Node::Kind::List, "", items);
        }
        default:
            throw ParseError{token.column, "expected a value, found '" + describe(token) + "'"};
        }
    }

    std::vector<Token> _tokens;
    size_t _position = 0;
    std::vector<Path>& _references;
};

std::optional<int> order(const Value& left, const Value& right) {
    const auto leftNumber = asNumber(left), rightNumber = asNumber(right);
    if (leftNumber && rightNumber) return *leftNumber < *rightNumber ? -1 : *leftNumber > *rightNumber ? 1 : 0;
    if (left.kind_case() == Value::kStringValue && right.kind_case() == Value::kStringValue)
        return left.string_value().compare(right.string_value());
    return std::nullopt;
}

bool contains(const Value& left, const Value& right) {
    if (right.kind_case() == Value::kListValue) {
        for (const auto& item : right.list_value().values()) if (valuesEqual(left, item)) return true;
        return false;
    }
    if (left.kind_case() == Value::kListValue) {
        for (const auto& item : left.list_value().values()) if (valuesEqual(item, right)) return true;
        return false;
    }
    if (left.kind_case() == Value::kStringValue && right.kind_case() == Value::kStringValue)
        return lowered(left.string_value()).find(lowered(right.string_value())) != std::string::npos;
    return false;
}

Value arithmetic(const std::string& op, const Value& left, const Value& right) {
    if (op == "+" || op == "-") {
        const auto leftNumber = isNull(left) ? std::optional<double>(0) : asNumber(left);
        const auto rightNumber = isNull(right) ? std::optional<double>(0) : asNumber(right);
        if (leftNumber && rightNumber) return numberValue(op == "+" ? *leftNumber + *rightNumber : *leftNumber - *rightNumber);
        if (op == "+") return stringValue(toText(left) + toText(right));   // "Hi " + $session.name
        return nullValue();
    }
    const auto leftNumber = asNumber(left), rightNumber = asNumber(right);
    if (!leftNumber || !rightNumber) return nullValue();
    if (op == "*") return numberValue(*leftNumber * *rightNumber);
    if (*rightNumber == 0) return nullValue();
    if (op == "/") return numberValue(*leftNumber / *rightNumber);
    return numberValue(std::fmod(*leftNumber, *rightNumber));
}

double rounded(double number, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(number * scale) / scale;
}

class Evaluator {
public:
    Evaluator(const Lookup& lookup, std::optional<TimePoint> now) : _lookup(lookup), _now(now) {}

    Value evaluate(const Node& node) {
        switch (node.kind) {
        case Node::Kind::Literal: return node.literal;
        case Node::Kind::Reference: return _lookup ? _lookup(node.path) : nullValue();
        case Node::Kind::List: {
            Value list;
            auto* values = list.mutable_list_value();
            for (const auto& child : node.children) *values->add_values() = evaluate(*child);
            return list;
        }
        case Node::Kind::Not: return boolValue(!isTruthy(evaluate(*node.children[0])));
        case Node::Kind::And: return boolValue(isTruthy(evaluate(*node.children[0])) && isTruthy(evaluate(*node.children[1])));
        case Node::Kind::Or: return boolValue(isTruthy(evaluate(*node.children[0])) || isTruthy(evaluate(*node.children[1])));
        case Node::Kind::Compare: {
            const Value left = evaluate(*node.children[0]);
            const Value right = evaluate(*node.children[1]);
            if (node.name == "=") return boolValue(valuesEqual(left, right));
            if (node.name == "!=") return boolValue(!valuesEqual(left, right));
            if (node.name == ":") return boolValue(contains(left, right));
            const auto comparison = order(left, right);
            if (!comparison) return boolValue(false);
            if (node.name == "<") return boolValue(*comparison < 0);
            if (node.name == "<=") return boolValue(*comparison <= 0);
            if (node.name == ">") return boolValue(*comparison > 0);
            return boolValue(*comparison >= 0);
        }
        case Node::Kind::Arithmetic: return arithmetic(node.name, evaluate(*node.children[0]), evaluate(*node.children[1]));
        case Node::Kind::Negate: {
            const auto number = asNumber(evaluate(*node.children[0]));
            return number ? numberValue(-*number) : nullValue();
        }
        case Node::Kind::Call: {
            std::vector<Value> arguments;
            for (const auto& child : node.children) arguments.push_back(evaluate(*child));
            return call(node.name, arguments);
        }
        }
        return nullValue();
    }

private:
    TimePoint now() {
        if (!_now) _now = std::chrono::system_clock::now();
        return *_now;
    }
    long today() { return nlu::english::daysOf(nlu::english::dateAt(now())); }

    Value call(const std::string& name, const std::vector<Value>& arguments) {
        if (name == "now") return stringValue(localDateTime(now()));
        if (name == "today") return stringValue(nlu::english::iso(nlu::english::civilFromDays(today())));
        const Value& argument = arguments[0];
        if (name == "exists") return boolValue(!isNull(argument));
        if (name == "empty") {
            switch (argument.kind_case()) {
            case Value::kStringValue: return boolValue(argument.string_value().empty());
            case Value::kListValue: return boolValue(argument.list_value().values().empty());
            case Value::kStructValue: return boolValue(argument.struct_value().fields().empty());
            default: return boolValue(isNull(argument));
            }
        }
        if (name == "len") {
            switch (argument.kind_case()) {
            case Value::kStringValue: return numberValue(static_cast<double>(argument.string_value().size()));
            case Value::kListValue: return numberValue(argument.list_value().values_size());
            case Value::kStructValue: return numberValue(static_cast<double>(argument.struct_value().fields().size()));
            default: return numberValue(0);
            }
        }
        if (name == "lower" || name == "upper") {
            std::string text = toText(argument);
            std::transform(text.begin(), text.end(), text.begin(),
                           [&](unsigned char c) { return name == "lower" ? std::tolower(c) : std::toupper(c); });
            return stringValue(text);
        }
        if (name == "days_until" || name == "days_since" || name == "weekday" || name == "add_days") {
            const auto days = asDays(argument);
            if (!days) return nullValue();
            if (name == "days_until") return numberValue(static_cast<double>(*days - today()));
            if (name == "days_since") return numberValue(static_cast<double>(today() - *days));
            if (name == "weekday") return numberValue((nlu::english::weekdayOf(*days) + 6) % 7 + 1);
            const auto count = asNumber(arguments[1]);
            if (!count) return nullValue();
            return stringValue(nlu::english::iso(nlu::english::civilFromDays(*days + static_cast<long>(std::lround(*count)))));
        }
        if (name == "min" || name == "max") {
            std::vector<Value> values;   // min($session.prices) takes a list's items
            if (arguments.size() == 1 && argument.kind_case() == Value::kListValue)
                values.assign(argument.list_value().values().begin(), argument.list_value().values().end());
            else
                values = arguments;
            std::optional<double> best;
            for (const auto& value : values) {
                const auto number = asNumber(value);
                if (!number) continue;
                if (!best || (name == "min" ? *number < *best : *number > *best)) best = number;
            }
            return best ? numberValue(*best) : nullValue();
        }
        const auto number = asNumber(argument);
        if (!number) return nullValue();
        if (name == "number") return numberValue(*number);
        if (name == "floor") return numberValue(std::floor(*number));
        if (name == "ceil") return numberValue(std::ceil(*number));
        if (name == "abs") return numberValue(std::fabs(*number));
        // round
        const auto digits = arguments.size() > 1 ? asNumber(arguments[1]) : std::optional<double>(0);
        return numberValue(rounded(*number, digits ? static_cast<int>(*digits) : 0));
    }

    const Lookup& _lookup;
    std::optional<TimePoint> _now;
};

} // namespace

std::shared_ptr<const Expression> Expression::parse(const std::string& text, std::string* error) {
    auto expression = std::make_shared<Expression>();
    try {
        Parser parser(tokenize(text), expression->_references);
        expression->_root = parser.parseAll();
    } catch (const ParseError& failure) {
        if (error) *error = "column " + std::to_string(failure.column) + ": " + failure.message;
        return nullptr;
    }
    return expression;
}

Value Expression::evaluate(const Lookup& lookup, std::optional<TimePoint> now) const {
    return Evaluator(lookup, now).evaluate(*_root);
}
bool Expression::test(const Lookup& lookup, std::optional<TimePoint> now) const { return isTruthy(evaluate(lookup, now)); }
const Path* Expression::reference() const {
    return _root->kind == Node::Kind::Reference ? &_root->path : nullptr;
}

bool isTruthy(const Value& value) {
    switch (value.kind_case()) {
    case Value::kBoolValue: return value.bool_value();
    case Value::kNumberValue: return value.number_value() != 0;
    case Value::kStringValue: return !value.string_value().empty();
    case Value::kListValue: return value.list_value().values_size() > 0;
    case Value::kStructValue: return !value.struct_value().fields().empty();
    default: return false;
    }
}

std::string toText(const Value& value) {
    switch (value.kind_case()) {
    case Value::kBoolValue: return value.bool_value() ? "true" : "false";
    case Value::kStringValue: return value.string_value();
    case Value::kNumberValue: {
        const double number = value.number_value();
        char buffer[32];
        if (std::isfinite(number) && std::floor(number) == number && std::fabs(number) < 1e15)
            std::snprintf(buffer, sizeof buffer, "%.0f", number);
        else
            std::snprintf(buffer, sizeof buffer, "%.10g", number);   // 0.1 + 0.2 reads as 0.3
        return buffer;
    }
    case Value::kListValue: {
        const auto& items = value.list_value().values();
        std::string text;
        for (int i = 0; i < items.size(); ++i) {
            if (i > 0) text += i + 1 == items.size() ? " and " : ", ";
            text += toText(items.Get(i));
        }
        return text;
    }
    default: return "";
    }
}

bool valuesEqual(const Value& left, const Value& right) {
    if (isNull(left) || isNull(right)) return isNull(left) && isNull(right);
    if (left.kind_case() == Value::kNumberValue || right.kind_case() == Value::kNumberValue) {
        const auto leftNumber = asNumber(left), rightNumber = asNumber(right);
        return leftNumber && rightNumber && *leftNumber == *rightNumber;
    }
    if (left.kind_case() != right.kind_case()) return false;
    switch (left.kind_case()) {
    case Value::kStringValue: return left.string_value() == right.string_value();
    case Value::kBoolValue: return left.bool_value() == right.bool_value();
    case Value::kListValue: {
        const auto& a = left.list_value().values();
        const auto& b = right.list_value().values();
        if (a.size() != b.size()) return false;
        for (int i = 0; i < a.size(); ++i) if (!valuesEqual(a.Get(i), b.Get(i))) return false;
        return true;
    }
    case Value::kStructValue: {
        const auto& a = left.struct_value().fields();
        const auto& b = right.struct_value().fields();
        if (a.size() != b.size()) return false;
        for (const auto& [name, item] : a) {
            const auto other = b.find(name);
            if (other == b.end() || !valuesEqual(item, other->second)) return false;
        }
        return true;
    }
    default: return false;
    }
}

Value valueAt(const Value& root, const Path& path, size_t from) {
    const Value* current = &root;
    for (size_t i = from; i < path.size(); ++i) {
        const std::string& part = path[i];
        if (current->kind_case() == Value::kStructValue) {
            const auto& fields = current->struct_value().fields();
            const auto found = fields.find(part);
            if (found == fields.end()) return nullValue();
            current = &found->second;
        } else if (current->kind_case() == Value::kListValue && !part.empty() &&
                   std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); })) {
            const size_t index = std::stoul(part);
            if (index >= static_cast<size_t>(current->list_value().values_size())) return nullValue();
            current = &current->list_value().values(static_cast<int>(index));
        } else {
            return nullValue();
        }
    }
    return *current;
}

std::string pathText(const Path& path) {
    std::string text = "$";
    for (size_t i = 0; i < path.size(); ++i) {
        const bool index = !path[i].empty() && std::isdigit(static_cast<unsigned char>(path[i][0]));
        if (index) text += "[" + path[i] + "]";
        else text += (i == 0 ? "" : ".") + path[i];
    }
    return text;
}

//  Templates

namespace {

// A filter after "|": its name and the values after its ":"s.
struct Filter {
    std::string name;
    std::vector<Value> arguments;
};

// What a ${...} holds: a reference or an expression, and its filters.
struct Placeholder {
    Path path;
    std::shared_ptr<const Expression> expression;
    std::vector<Filter> filters;
    bool readsValue = false;   // a filter needs the value, not how it's said
};

// Where `text` splits on `separator` outside quotes and parentheses.
std::vector<std::string> splitOutside(const std::string& text, char separator) {
    std::vector<std::string> parts(1);
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == '\\' && i + 1 < text.size()) parts.back() += text[i++];
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == separator && depth == 0) {
            parts.emplace_back();
            continue;
        }
        parts.back() += c;
    }
    return parts;
}

// {fewest, most} values a filter takes; nullopt for one that isn't a filter.
std::optional<std::pair<size_t, size_t>> filterArity(const std::string& name) {
    if (name == "raw" || name == "count" || name == "lower" || name == "upper" || name == "capitalize") return std::pair<size_t, size_t>{0, 0};
    if (name == "round") return std::pair<size_t, size_t>{0, 1};
    if (name == "default") return std::pair<size_t, size_t>{1, 1};
    if (name == "plural") return std::pair<size_t, size_t>{2, 2};
    return std::nullopt;
}

// "plural:\"car\":\"cars\"" → {plural, ["car", "cars"]}. An error names what's wrong.
std::optional<Filter> parseFilter(const std::string& text, std::string& error) {
    const auto parts = splitOutside(text, ':');
    Filter filter;
    filter.name = trimmed(parts[0]);
    const auto takes = filterArity(filter.name);
    if (!takes) {
        error = "unknown filter '" + filter.name + "' (raw, count, plural, round, lower, upper, capitalize or default)";
        return std::nullopt;
    }
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string argument = trimmed(parts[i]);
        if (argument.size() >= 2 && (argument.front() == '"' || argument.front() == '\'') && argument.back() == argument.front()) {
            filter.arguments.push_back(stringValue(argument.substr(1, argument.size() - 2)));
        } else if (const auto number = asNumber(stringValue(argument))) {
            filter.arguments.push_back(numberValue(*number));
        } else {
            error = "filter '" + filter.name + "': " + (argument.empty() ? "an empty value" : "'" + argument + "' needs quotes");
            return std::nullopt;
        }
    }
    if (filter.arguments.size() < takes->first || filter.arguments.size() > takes->second) {
        error = "filter '" + filter.name + "' takes " +
                (takes->second == 0 ? std::string("no values")
                 : takes->first == takes->second ? std::to_string(takes->first) + (takes->first == 1 ? " value" : " values")
                                                 : "at most " + std::to_string(takes->second) + " value");
        return std::nullopt;
    }
    return filter;
}

// The inside of ${...}: "session.points", "session.cars|count", "$session.points * 2|round".
std::optional<Placeholder> parsePlaceholder(const std::string& inside, std::string& error) {
    const auto parts = splitOutside(inside, '|');
    const std::string first = trimmed(parts[0]);
    Placeholder placeholder;
    if (first.empty()) {
        error = "empty ${}";
        return std::nullopt;
    }
    if (readPath(first, 0, placeholder.path) != first.size()) {
        placeholder.path.clear();
        std::string parseError;
        placeholder.expression = Expression::parse(first, &parseError);
        if (!placeholder.expression) {
            error = "${" + first + "}: " + parseError;
            return std::nullopt;
        }
    }
    for (size_t i = 1; i < parts.size(); ++i) {
        std::string filterError;
        auto filter = parseFilter(parts[i], filterError);
        if (!filter) {
            error = "${" + trimmed(inside) + "}: " + filterError;
            return std::nullopt;
        }
        placeholder.readsValue = placeholder.readsValue || filter->name == "raw" || filter->name == "count" ||
                                 filter->name == "plural" || filter->name == "round";
        placeholder.filters.push_back(std::move(*filter));
    }
    return placeholder;
}

double countOf(const Value& value) {
    switch (value.kind_case()) {
    case Value::kListValue: return value.list_value().values_size();
    case Value::kStructValue: return static_cast<double>(value.struct_value().fields().size());
    case Value::kNullValue:
    case Value::KIND_NOT_SET: return 0;
    case Value::kStringValue: return value.string_value().empty() ? 0 : 1;
    default: return 1;
    }
}

Value applyFilter(const Filter& filter, const Value& value) {
    if (filter.name == "raw") return value;
    if (filter.name == "count") return numberValue(countOf(value));
    if (filter.name == "plural") {
        const auto number = value.kind_case() == Value::kListValue ? std::optional<double>(countOf(value)) : asNumber(value);
        const double count = number.value_or(0);
        return stringValue(toText(numberValue(count)) + " " + toText(filter.arguments[count == 1 ? 0 : 1]));
    }
    if (filter.name == "round") {
        const auto number = asNumber(value);
        if (!number) return value;
        const int digits = filter.arguments.empty() ? 0 : static_cast<int>(filter.arguments[0].number_value());
        return numberValue(rounded(*number, digits));
    }
    if (filter.name == "default") return isTruthy(value) || asNumber(value) ? value : filter.arguments[0];
    std::string text = toText(value);
    if (filter.name == "capitalize") {
        if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    } else {
        std::transform(text.begin(), text.end(), text.begin(),
                       [&](unsigned char c) { return filter.name == "lower" ? std::tolower(c) : std::toupper(c); });
    }
    return stringValue(text);
}

// Where the "}" closing a "${" at `open` is, outside quotes; npos when there's none.
size_t closingBrace(const std::string& text, size_t open) {
    char quote = 0;
    for (size_t i = open + 2; i < text.size(); ++i) {
        const char c = text[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '}') {
            return i;
        }
    }
    return std::string::npos;
}

// Walks a template, calling `onText` for literal text and `onPlaceholder` for each reference.
bool scanTemplate(const std::string& text, const std::function<void(const std::string&)>& onText,
                  const std::function<void(const Placeholder&)>& onPlaceholder, std::string* error) {
    size_t i = 0;
    while (i < text.size()) {
        const size_t dollar = text.find('$', i);
        if (dollar == std::string::npos) {
            onText(text.substr(i));
            break;
        }
        onText(text.substr(i, dollar - i));
        if (dollar + 1 < text.size() && text[dollar + 1] == '$') {
            onText("$");
            i = dollar + 2;
        } else if (dollar + 1 < text.size() && text[dollar + 1] == '{') {
            const size_t close = closingBrace(text, dollar);
            if (close == std::string::npos) {
                if (error) *error = "column " + std::to_string(dollar + 1) + ": ${ without a closing }";
                return false;
            }
            std::string placeholderError;
            const auto placeholder = parsePlaceholder(text.substr(dollar + 2, close - dollar - 2), placeholderError);
            if (!placeholder) {
                if (error) *error = "column " + std::to_string(dollar + 1) + ": " + placeholderError;
                return false;
            }
            onPlaceholder(*placeholder);
            i = close + 1;
        } else {
            Placeholder placeholder;
            const size_t end = readPath(text, dollar + 1, placeholder.path);
            if (end == std::string::npos) {   // "$5": just a dollar sign
                onText("$");
                i = dollar + 1;
            } else {
                onPlaceholder(placeholder);
                i = end;
            }
        }
    }
    return true;
}

} // namespace

std::string renderTemplate(const std::string& text, const TemplateScope& scope) {
    const Lookup& raw = scope.raw ? scope.raw : scope.spoken;
    std::string rendered;
    scanTemplate(
        text, [&](const std::string& part) { rendered += part; },
        [&](const Placeholder& placeholder) {
            Value value;
            if (placeholder.expression) value = placeholder.expression->evaluate(raw, scope.now);
            else if (const Lookup& lookup = placeholder.readsValue ? raw : scope.spoken; lookup) value = lookup(placeholder.path);
            for (const auto& filter : placeholder.filters) value = applyFilter(filter, value);
            rendered += toText(value);
        },
        nullptr);
    return rendered;
}

std::string renderTemplate(const std::string& text, const Lookup& lookup) {
    return renderTemplate(text, TemplateScope{lookup, nullptr, std::nullopt});
}

bool templateReferences(const std::string& text, std::vector<Path>& references, std::string* error) {
    return scanTemplate(
        text, [](const std::string&) {},
        [&](const Placeholder& placeholder) {
            if (placeholder.expression)
                references.insert(references.end(), placeholder.expression->references().begin(),
                                  placeholder.expression->references().end());
            else
                references.push_back(placeholder.path);
        },
        error);
}

} // namespace flow
} // namespace va
