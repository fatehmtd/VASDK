#include "flow/Expression.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
                   (c == '-' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
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
        } else {
            static const char* operators[] = {"!=", "<=", ">=", "==", "=", "<", ">", ":"};
            bool found = false;
            for (const char* op : operators) {
                const size_t length = std::char_traits<char>::length(op);
                if (text.compare(i, length, op) == 0) {
                    token.type = Token::Type::Operator;
                    token.text = std::string(op) == "==" ? "=" : op;
                    i += length;
                    found = true;
                    break;
                }
            }
            if (!found) throw ParseError{i + 1, std::string("unexpected '") + c + "'"};
        }
        tokens.push_back(token);
    }
}

} // namespace

struct Expression::Node {
    enum class Kind { Literal, Reference, List, Not, And, Or, Compare, Call };
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
        auto left = parseOperand();
        if (peek().type != Token::Type::Operator) return left;
        const std::string op = take().text;
        return make(Node::Kind::Compare, op, {left, parseOperand()});
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
            static const char* functions[] = {"exists", "empty", "len", "lower", "upper"};
            if (std::find_if(std::begin(functions), std::end(functions),
                             [&](const char* name) { return word == name; }) == std::end(functions))
                throw ParseError{token.column, "unknown word '" + token.text + "' (strings need quotes)"};
            if (take().type != Token::Type::OpenParenthesis) throw ParseError{token.column, "expected '(' after " + word};
            auto argument = parseOr();
            if (take().type != Token::Type::CloseParenthesis) throw ParseError{token.column, word + " takes one value, then ')'"};
            return make(Node::Kind::Call, word, {argument});
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

Value evaluateNode(const Node& node, const Lookup& lookup) {
    switch (node.kind) {
    case Node::Kind::Literal: return node.literal;
    case Node::Kind::Reference: return lookup ? lookup(node.path) : nullValue();
    case Node::Kind::List: {
        Value list;
        auto* values = list.mutable_list_value();
        for (const auto& child : node.children) *values->add_values() = evaluateNode(*child, lookup);
        return list;
    }
    case Node::Kind::Not: return boolValue(!isTruthy(evaluateNode(*node.children[0], lookup)));
    case Node::Kind::And:
        return boolValue(isTruthy(evaluateNode(*node.children[0], lookup)) &&
                         isTruthy(evaluateNode(*node.children[1], lookup)));
    case Node::Kind::Or:
        return boolValue(isTruthy(evaluateNode(*node.children[0], lookup)) ||
                         isTruthy(evaluateNode(*node.children[1], lookup)));
    case Node::Kind::Compare: {
        const Value left = evaluateNode(*node.children[0], lookup);
        const Value right = evaluateNode(*node.children[1], lookup);
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
    case Node::Kind::Call: {
        const Value argument = evaluateNode(*node.children[0], lookup);
        if (node.name == "exists") return boolValue(!isNull(argument));
        if (node.name == "empty") {
            switch (argument.kind_case()) {
            case Value::kStringValue: return boolValue(argument.string_value().empty());
            case Value::kListValue: return boolValue(argument.list_value().values().empty());
            case Value::kStructValue: return boolValue(argument.struct_value().fields().empty());
            default: return boolValue(isNull(argument));
            }
        }
        if (node.name == "len") {
            switch (argument.kind_case()) {
            case Value::kStringValue: return numberValue(static_cast<double>(argument.string_value().size()));
            case Value::kListValue: return numberValue(argument.list_value().values_size());
            case Value::kStructValue: return numberValue(static_cast<double>(argument.struct_value().fields().size()));
            default: return numberValue(0);
            }
        }
        std::string text = toText(argument);
        std::transform(text.begin(), text.end(), text.begin(), [&](unsigned char c) {
            return node.name == "lower" ? std::tolower(c) : std::toupper(c);
        });
        return stringValue(text);
    }
    }
    return nullValue();
}

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

Value Expression::evaluate(const Lookup& lookup) const { return evaluateNode(*_root, lookup); }
bool Expression::test(const Lookup& lookup) const { return isTruthy(evaluate(lookup)); }
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
            std::snprintf(buffer, sizeof buffer, "%g", number);
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

namespace {
// Walks a template, calling `onText` for literal text and `onReference` for each reference.
bool scanTemplate(const std::string& text, const std::function<void(const std::string&)>& onText,
                  const std::function<void(const Path&)>& onReference, std::string* error) {
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
            const size_t close = text.find('}', dollar);
            Path path;
            if (close == std::string::npos || readPath(text, dollar + 2, path) != close) {
                if (error) *error = "column " + std::to_string(dollar + 1) + ": expected ${name} with a closing }";
                return false;
            }
            onReference(path);
            i = close + 1;
        } else {
            Path path;
            const size_t end = readPath(text, dollar + 1, path);
            if (end == std::string::npos) {   // "$5": just a dollar sign
                onText("$");
                i = dollar + 1;
            } else {
                onReference(path);
                i = end;
            }
        }
    }
    return true;
}
} // namespace

std::string renderTemplate(const std::string& text, const Lookup& lookup) {
    std::string rendered;
    scanTemplate(text, [&](const std::string& part) { rendered += part; },
                 [&](const Path& path) { if (lookup) rendered += toText(lookup(path)); }, nullptr);
    return rendered;
}

bool templateReferences(const std::string& text, std::vector<Path>& references, std::string* error) {
    return scanTemplate(text, [](const std::string&) {}, [&](const Path& path) { references.push_back(path); }, error);
}

} // namespace flow
} // namespace va
