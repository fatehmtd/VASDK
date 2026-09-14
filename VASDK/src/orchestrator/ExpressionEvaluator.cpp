#include <VASDK/orchestrator/ExpressionEvaluator.h>

#include <cctype>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

namespace va {
namespace orchestrator {

namespace {

//  Tokenizer

struct Token {
    enum class Kind { Ident, Number, String, Op, End };
    Kind kind = Kind::End;
    std::string text;
};

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    size_t i = 0, n = s.size();
    auto isIdentStart = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; };
    auto isIdentChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    while (i < n) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (isIdentStart(c)) {
            size_t start = i;
            while (i < n && isIdentChar(s[i])) ++i;
            tokens.push_back({Token::Kind::Ident, s.substr(start, i - start)});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            while (i < n && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            tokens.push_back({Token::Kind::Number, s.substr(start, i - start)});
            continue;
        }
        if (c == '"') {
            ++i;
            std::string value;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) {
                    value.push_back(s[i + 1]);
                    i += 2;
                } else {
                    value.push_back(s[i]);
                    ++i;
                }
            }
            if (i >= n) throw std::runtime_error("[ExpressionEvaluator] unterminated string literal in: " + s);
            ++i;   // closing quote
            tokens.push_back({Token::Kind::String, value});
            continue;
        }
        if (i + 1 < n) {
            std::string two = s.substr(i, 2);
            if (two == "==" || two == "!=" || two == ">=" || two == "<=" || two == "&&" || two == "||") {
                tokens.push_back({Token::Kind::Op, two});
                i += 2;
                continue;
            }
        }
        if (std::string("+-*/()<>!.,").find(c) != std::string::npos) {
            tokens.push_back({Token::Kind::Op, std::string(1, c)});
            ++i;
            continue;
        }
        throw std::runtime_error(std::string("[ExpressionEvaluator] unexpected character '") + c + "' in: " + s);
    }
    tokens.push_back({Token::Kind::End, ""});
    return tokens;
}

//  AST

struct Node {
    enum class Kind { Number, String, Bool, Field, Neg, Not, BinOp, Has, Contains };
    Kind kind = Kind::Number;
    double number = 0;
    std::string text;   // String literal value, BinOp operator token, or Field/Has dotted path
    bool boolean = false;
    std::shared_ptr<Node> a, b;   // a = operand/left/receiver; b = right/arg
};

std::shared_ptr<Node> makeNode(Node::Kind kind) {
    auto node = std::make_shared<Node>();
    node->kind = kind;
    return node;
}

//  Recursive-descent parser (precedence climbing)

class ExprParser {
public:
    explicit ExprParser(const std::string& text) : _text(text), _tokens(tokenize(text)) {}

    std::shared_ptr<Node> parse() {
        std::shared_ptr<Node> root = parseOr();
        if (current().kind != Token::Kind::End) {
            fail("unexpected trailing token '" + current().text + "'");
        }
        return root;
    }

private:
    const std::string& _text;
    std::vector<Token> _tokens;
    size_t _pos = 0;

    const Token& current() const { return _tokens[_pos]; }
    void advance() { if (_pos + 1 < _tokens.size()) ++_pos; }

    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error("[ExpressionEvaluator] malformed expression \"" + _text + "\": " + reason);
    }

    bool atOp(const std::string& op) const { return current().kind == Token::Kind::Op && current().text == op; }

    void expectOp(const std::string& op) {
        if (!atOp(op)) fail("expected '" + op + "', got '" + current().text + "'");
        advance();
    }

    std::shared_ptr<Node> parseOr() {
        auto left = parseAnd();
        while (atOp("||")) {
            advance();
            auto node = makeNode(Node::Kind::BinOp);
            node->text = "||";
            node->a = left;
            node->b = parseAnd();
            left = node;
        }
        return left;
    }

    std::shared_ptr<Node> parseAnd() {
        auto left = parseComparison();
        while (atOp("&&")) {
            advance();
            auto node = makeNode(Node::Kind::BinOp);
            node->text = "&&";
            node->a = left;
            node->b = parseComparison();
            left = node;
        }
        return left;
    }

    std::shared_ptr<Node> parseComparison() {
        auto left = parseAdditive();
        static const std::vector<std::string> ops = {"==", "!=", ">=", "<=", ">", "<"};
        for (const auto& op : ops) {
            if (atOp(op)) {
                advance();
                auto node = makeNode(Node::Kind::BinOp);
                node->text = op;
                node->a = left;
                node->b = parseAdditive();
                return node;
            }
        }
        return left;
    }

    std::shared_ptr<Node> parseAdditive() {
        auto left = parseMultiplicative();
        while (atOp("+") || atOp("-")) {
            std::string op = current().text;
            advance();
            auto node = makeNode(Node::Kind::BinOp);
            node->text = op;
            node->a = left;
            node->b = parseMultiplicative();
            left = node;
        }
        return left;
    }

    std::shared_ptr<Node> parseMultiplicative() {
        auto left = parseUnary();
        while (atOp("*") || atOp("/")) {
            std::string op = current().text;
            advance();
            auto node = makeNode(Node::Kind::BinOp);
            node->text = op;
            node->a = left;
            node->b = parseUnary();
            left = node;
        }
        return left;
    }

    std::shared_ptr<Node> parseUnary() {
        if (atOp("!")) {
            advance();
            auto node = makeNode(Node::Kind::Not);
            node->a = parseUnary();
            return node;
        }
        if (atOp("-")) {
            advance();
            auto node = makeNode(Node::Kind::Neg);
            node->a = parseUnary();
            return node;
        }
        return parsePrimary();
    }

    std::shared_ptr<Node> parsePrimary() {
        const Token& tok = current();

        if (tok.kind == Token::Kind::Number) {
            auto node = makeNode(Node::Kind::Number);
            node->number = std::strtod(tok.text.c_str(), nullptr);
            advance();
            return node;
        }
        if (tok.kind == Token::Kind::String) {
            auto node = makeNode(Node::Kind::String);
            node->text = tok.text;
            advance();
            return node;
        }
        if (atOp("(")) {
            advance();
            auto inner = parseOr();
            expectOp(")");
            return inner;
        }
        if (tok.kind == Token::Kind::Ident) {
            if (tok.text == "true" || tok.text == "false") {
                auto node = makeNode(Node::Kind::Bool);
                node->boolean = (tok.text == "true");
                advance();
                return node;
            }
            if (tok.text == "has") {
                size_t save = _pos;
                advance();
                if (atOp("(")) {
                    advance();
                    std::string path = parseFieldPath();
                    expectOp(")");
                    auto node = makeNode(Node::Kind::Has);
                    node->text = path;
                    return node;
                }
                _pos = save;   // "has" used as an ordinary field name, not a call
            }
            return parseFieldPathOrContains();
        }
        fail("unexpected token '" + tok.text + "'");
    }

    // A plain dotted path (Field), unless a ".contains(" appears partway
    // through, which turns it into a method call on the path read so far.
    std::shared_ptr<Node> parseFieldPathOrContains() {
        if (current().kind != Token::Kind::Ident) fail("expected an identifier, got '" + current().text + "'");
        std::string path = current().text;
        advance();

        while (atOp(".")) {
            advance();   // '.'
            if (current().kind != Token::Kind::Ident) fail("expected a name after '.'");
            std::string segment = current().text;
            size_t afterIdent = _pos + 1;
            bool isCall = segment == "contains" && afterIdent < _tokens.size() &&
                          _tokens[afterIdent].kind == Token::Kind::Op && _tokens[afterIdent].text == "(";
            if (isCall) {
                advance();       // consume "contains"
                expectOp("(");
                auto receiver = makeNode(Node::Kind::Field);
                receiver->text = path;
                auto node = makeNode(Node::Kind::Contains);
                node->a = receiver;
                node->b = parseOr();
                expectOp(")");
                return node;
            }
            advance();   // consume the segment identifier
            path += "." + segment;
        }

        auto node = makeNode(Node::Kind::Field);
        node->text = path;
        return node;
    }

    std::string parseFieldPath() {
        if (current().kind != Token::Kind::Ident) fail("expected an identifier, got '" + current().text + "'");
        std::string path = current().text;
        advance();
        while (atOp(".")) {
            advance();
            if (current().kind != Token::Kind::Ident) fail("expected a name after '.'");
            path += "." + current().text;
            advance();
        }
        return path;
    }
};

//  Evaluation

using Activation = ExpressionEvaluator::Activation;

const std::map<std::string, google::protobuf::Value>* namespaceMap(const std::string& ns, const Activation& act) {
    if (ns == "nlu") return &act.nlu;
    if (ns == "context") return &act.context;
    if (ns == "shared") return &act.shared;
    if (ns == "event") return &act.event;
    return nullptr;
}

// Splits "nlu.amount" into ("nlu", "amount"); "current_volume" into ("", "current_volume").
std::pair<std::string, std::string> splitNamespace(const std::string& path) {
    size_t dot = path.find('.');
    if (dot == std::string::npos) return {"", path};
    return {path.substr(0, dot), path.substr(dot + 1)};
}

bool fieldPresent(const std::string& path, const Activation& act) {
    auto ns_key = splitNamespace(path);
    const std::string& ns = ns_key.first;
    const std::string& key = ns_key.second;
    if (ns.empty()) return act.parameters.count(key) != 0;
    if (ns == "entity") {
        throw std::runtime_error("[ExpressionEvaluator] 'entity.*' is not supported yet (see "
                                  "requestflow-roadmap.md's M1.4 entry) - referenced in: " + path);
    }
    const auto* map = namespaceMap(ns, act);
    if (!map) throw std::runtime_error("[ExpressionEvaluator] unknown namespace '" + ns + "' in: " + path);
    return map->count(key) != 0;
}

google::protobuf::Value lookupField(const std::string& path, const Activation& act) {
    auto ns_key = splitNamespace(path);
    const std::string& ns = ns_key.first;
    const std::string& key = ns_key.second;
    const std::map<std::string, google::protobuf::Value>* map = nullptr;
    if (ns.empty()) {
        map = &act.parameters;
    } else if (ns == "entity") {
        throw std::runtime_error("[ExpressionEvaluator] 'entity.*' is not supported yet (see "
                                  "requestflow-roadmap.md's M1.4 entry) - referenced in: " + path);
    } else {
        map = namespaceMap(ns, act);
        if (!map) throw std::runtime_error("[ExpressionEvaluator] unknown namespace '" + ns + "' in: " + path);
    }
    auto it = map->find(key);
    if (it == map->end()) {
        // Milestone 3.4 - FieldNotPresentError (not plain std::runtime_error)
        // so OrchestratorEngine::evaluateRoutesOnce()'s condition-route loop
        // can treat this specific case as "doesn't match" rather than crashing
        // the turn; see ExpressionEvaluator.h's class comment on the type.
        throw FieldNotPresentError("[ExpressionEvaluator] '" + path +
                                    "' is not present - guard with has(" + path + ") first if it may be absent");
    }
    return it->second;
}

double asNumber(const google::protobuf::Value& v, const std::string& exprText) {
    if (v.kind_case() != google::protobuf::Value::kNumberValue) {
        throw std::runtime_error("[ExpressionEvaluator] expected a number in: " + exprText);
    }
    return v.number_value();
}

bool asBool(const google::protobuf::Value& v, const std::string& exprText) {
    if (v.kind_case() != google::protobuf::Value::kBoolValue) {
        throw std::runtime_error("[ExpressionEvaluator] expected a boolean in: " + exprText);
    }
    return v.bool_value();
}

std::string asString(const google::protobuf::Value& v, const std::string& exprText) {
    if (v.kind_case() != google::protobuf::Value::kStringValue) {
        throw std::runtime_error("[ExpressionEvaluator] expected a string in: " + exprText);
    }
    return v.string_value();
}

bool valuesEqual(const google::protobuf::Value& a, const google::protobuf::Value& b) {
    if (a.kind_case() != b.kind_case()) return false;
    switch (a.kind_case()) {
        case google::protobuf::Value::kNumberValue: return a.number_value() == b.number_value();
        case google::protobuf::Value::kStringValue: return a.string_value() == b.string_value();
        case google::protobuf::Value::kBoolValue:    return a.bool_value() == b.bool_value();
        default: return false;
    }
}

google::protobuf::Value makeNumber(double d) {
    google::protobuf::Value v;
    v.set_number_value(d);
    return v;
}
google::protobuf::Value makeBool(bool b) {
    google::protobuf::Value v;
    v.set_bool_value(b);
    return v;
}
google::protobuf::Value makeString(const std::string& s) {
    google::protobuf::Value v;
    v.set_string_value(s);
    return v;
}

google::protobuf::Value evalNode(const Node* node, const Activation& act, const std::string& exprText) {
    switch (node->kind) {
        case Node::Kind::Number: return makeNumber(node->number);
        case Node::Kind::String: return makeString(node->text);
        case Node::Kind::Bool:   return makeBool(node->boolean);
        case Node::Kind::Field:  return lookupField(node->text, act);
        case Node::Kind::Has:    return makeBool(fieldPresent(node->text, act));
        case Node::Kind::Neg: {
            double v = asNumber(evalNode(node->a.get(), act, exprText), exprText);
            return makeNumber(-v);
        }
        case Node::Kind::Not: {
            bool v = asBool(evalNode(node->a.get(), act, exprText), exprText);
            return makeBool(!v);
        }
        case Node::Kind::Contains: {
            std::string receiver = asString(evalNode(node->a.get(), act, exprText), exprText);
            std::string arg = asString(evalNode(node->b.get(), act, exprText), exprText);
            return makeBool(receiver.find(arg) != std::string::npos);
        }
        case Node::Kind::BinOp: {
            const std::string& op = node->text;
            if (op == "&&") {
                if (!asBool(evalNode(node->a.get(), act, exprText), exprText)) return makeBool(false);
                return makeBool(asBool(evalNode(node->b.get(), act, exprText), exprText));
            }
            if (op == "||") {
                if (asBool(evalNode(node->a.get(), act, exprText), exprText)) return makeBool(true);
                return makeBool(asBool(evalNode(node->b.get(), act, exprText), exprText));
            }
            google::protobuf::Value left = evalNode(node->a.get(), act, exprText);
            google::protobuf::Value right = evalNode(node->b.get(), act, exprText);
            if (op == "==") return makeBool(valuesEqual(left, right));
            if (op == "!=") return makeBool(!valuesEqual(left, right));
            if (op == "+") return makeNumber(asNumber(left, exprText) + asNumber(right, exprText));
            if (op == "-") return makeNumber(asNumber(left, exprText) - asNumber(right, exprText));
            if (op == "*") return makeNumber(asNumber(left, exprText) * asNumber(right, exprText));
            if (op == "/") return makeNumber(asNumber(left, exprText) / asNumber(right, exprText));
            if (op == ">")  return makeBool(asNumber(left, exprText) >  asNumber(right, exprText));
            if (op == ">=") return makeBool(asNumber(left, exprText) >= asNumber(right, exprText));
            if (op == "<")  return makeBool(asNumber(left, exprText) <  asNumber(right, exprText));
            if (op == "<=") return makeBool(asNumber(left, exprText) <= asNumber(right, exprText));
            throw std::runtime_error("[ExpressionEvaluator] internal error: unknown operator '" + op + "'");
        }
    }
    throw std::runtime_error("[ExpressionEvaluator] internal error: unhandled node kind");
}

}   // namespace

struct ExpressionEvaluator::Impl {
    std::string text;
    std::shared_ptr<Node> root;
};

ExpressionEvaluator::ExpressionEvaluator(const std::string& text) : _impl(std::make_shared<Impl>()) {
    _impl->text = text;
    ExprParser parser(text);
    _impl->root = parser.parse();
}

google::protobuf::Value ExpressionEvaluator::evaluate(const Activation& activation) const {
    return evalNode(_impl->root.get(), activation, _impl->text);
}

}   // namespace orchestrator
}   // namespace va
