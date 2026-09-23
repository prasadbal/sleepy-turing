// A one-liner expression language for report columns, built on
// path_resolver.h's field access: numbers, +-*/, parentheses, and
// FUNC.name(args...) calls against a caller-supplied function table.
//
// Parsing and evaluation are deliberately separate steps: parse(expr) walks
// the string ONCE (at config-load time, for each of a schema's ~190
// columns) into a small AST; eval() walks that AST once PER ROW. A 1M-row
// report re-parsing every column's expression text for every row would be
// pure waste -- parse once, keep the Node, eval many times.
//
//   auto ast = posreport::parse("ins.notional * FUNC.fx_rate(ins.ccy)");
//   for (const auto& pos : positions) {
//       auto v = posreport::eval(*ast, pos, resolvers, funcs);
//   }
//
// Grammar:
//   expr       := comparison
//   comparison := additive (('=='|'!='|'<'|'<='|'>'|'>=') additive)?   -- at most one; "a < b < c" is a syntax error, not chained
//   additive   := term (('+' | '-') term)*
//   term       := unary (('*' | '/') unary)*
//   unary      := '-' unary | primary
//   primary    := NUMBER | STRING | path | '(' expr ')'
//               | 'IF' '(' expr ',' expr ',' expr ')'
//   path       := IDENT ('.' IDENT)*  [ '(' (expr (',' expr)*)? ')' ]
// A path is a function call exactly when its segments are ["FUNC", name]
// and it's followed by '(' -- FUNC is a reserved first segment, distinct
// from the Position/ins/db/Config roots path_resolver.h resolves. Anything
// else followed by '(' is a syntax error (paths aren't callable).
//
// IF(cond, then, else) is a language primitive, not a FUNC: a FUNC call
// evaluates every argument eagerly before the function runs (see Call in
// eval() below), so a function-based if(cond, a, b) would evaluate BOTH a
// and b on every row regardless of cond -- wrong (wasteful at best, wrong
// if the untaken branch is only valid when cond doesn't hold) and not fixable
// without a special case, so IF gets its own AST node with real short-circuit
// evaluation instead of going through the function table.
#pragma once
#include <posreport/path_resolver.h>

#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace posreport {

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------
struct Token {
    enum class Kind {
        Ident, Number, String, Dot, Plus, Minus, Star, Slash, LParen, RParen, Comma,
        Eq, Ne, Lt, Le, Gt, Ge, End
    } kind;
    std::string text; // Ident/String (String: the unescaped contents, no quotes)
    double      num = 0; // Number
};

inline std::vector<Token> tokenize(std::string_view s) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t') { ++i; continue; }
        if (c == '.') { out.push_back({Token::Kind::Dot, "."}); ++i; continue; }
        if (c == '+') { out.push_back({Token::Kind::Plus, "+"}); ++i; continue; }
        if (c == '-') { out.push_back({Token::Kind::Minus, "-"}); ++i; continue; }
        if (c == '*') { out.push_back({Token::Kind::Star, "*"}); ++i; continue; }
        if (c == '/') { out.push_back({Token::Kind::Slash, "/"}); ++i; continue; }
        if (c == '(') { out.push_back({Token::Kind::LParen, "("}); ++i; continue; }
        if (c == ')') { out.push_back({Token::Kind::RParen, ")"}); ++i; continue; }
        if (c == ',') { out.push_back({Token::Kind::Comma, ","}); ++i; continue; }
        if (c == '=' && i + 1 < s.size() && s[i + 1] == '=') { out.push_back({Token::Kind::Eq, "=="}); i += 2; continue; }
        if (c == '!' && i + 1 < s.size() && s[i + 1] == '=') { out.push_back({Token::Kind::Ne, "!="}); i += 2; continue; }
        if (c == '<' && i + 1 < s.size() && s[i + 1] == '=') { out.push_back({Token::Kind::Le, "<="}); i += 2; continue; }
        if (c == '>' && i + 1 < s.size() && s[i + 1] == '=') { out.push_back({Token::Kind::Ge, ">="}); i += 2; continue; }
        if (c == '<') { out.push_back({Token::Kind::Lt, "<"}); ++i; continue; }
        if (c == '>') { out.push_back({Token::Kind::Gt, ">"}); ++i; continue; }
        if (c == '"') {
            std::string val;
            std::size_t j = i + 1;
            for (; j < s.size() && s[j] != '"'; ++j) {
                if (s[j] == '\\' && j + 1 < s.size() && (s[j + 1] == '"' || s[j + 1] == '\\')) { val += s[j + 1]; ++j; }
                else val += s[j];
            }
            if (j >= s.size()) throw std::runtime_error("posreport: unterminated string literal in expression: " + std::string(s));
            out.push_back({Token::Kind::String, val});
            i = j + 1;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            out.push_back({Token::Kind::Ident, std::string(s.substr(i, j - i))});
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            Token t{Token::Kind::Number, std::string(s.substr(i, j - i))};
            t.num = std::stod(t.text);
            out.push_back(t);
            i = j;
            continue;
        }
        throw std::runtime_error("posreport: unexpected character '" + std::string(1, c) + "' in expression: " + std::string(s));
    }
    out.push_back({Token::Kind::End, ""});
    return out;
}

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------
enum class Op { Add, Sub, Mul, Div };
enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge };

struct Node {
    enum class Kind { Literal, Path, BinOp, Neg, Call, Cmp, If } kind;
    Value                     literal;   // Literal
    std::vector<std::string>  path;      // Path
    Op                        op = Op::Add;     // BinOp
    CmpOp                     cmp = CmpOp::Eq;  // Cmp
    std::unique_ptr<Node>     lhs, rhs;  // BinOp/Cmp (both), Neg (lhs only)
    std::string               func_name; // Call
    std::vector<std::unique_ptr<Node>> args; // Call
    std::unique_ptr<Node>     cond, then_branch, else_branch; // If
};
using NodePtr = std::unique_ptr<Node>;

// ---------------------------------------------------------------------------
// Parser -- precedence climbing, single pass over the token list.
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::string_view src) : src_(src), toks_(tokenize(src)) {}

    NodePtr parse_all() {
        auto n = parse_comparison();
        expect(Token::Kind::End, "trailing input after a complete expression");
        return n;
    }

private:
    std::string_view    src_;
    std::vector<Token>   toks_;
    std::size_t          pos_ = 0;

    const Token& peek() const { return toks_[pos_]; }
    Token take() { return toks_[pos_++]; }
    void expect(Token::Kind k, const char* what) {
        if (peek().kind != k) fail(what);
    }
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("posreport: " + what + " in expression: " + std::string(src_));
    }

    NodePtr parse_comparison() { // at most one -- "a < b < c" is a syntax error, not chained
        auto n = parse_expr();
        static const std::pair<Token::Kind, CmpOp> ops[] = {
            {Token::Kind::Eq, CmpOp::Eq}, {Token::Kind::Ne, CmpOp::Ne}, {Token::Kind::Le, CmpOp::Le},
            {Token::Kind::Ge, CmpOp::Ge}, {Token::Kind::Lt, CmpOp::Lt}, {Token::Kind::Gt, CmpOp::Gt},
        };
        for (const auto& [tk, cmp] : ops) {
            if (peek().kind == tk) {
                take();
                auto node = std::make_unique<Node>();
                node->kind = Node::Kind::Cmp; node->cmp = cmp; node->lhs = std::move(n); node->rhs = parse_expr();
                return node;
            }
        }
        return n;
    }

    NodePtr parse_expr() { // + -
        auto n = parse_term();
        for (;;) {
            if (peek().kind == Token::Kind::Plus || peek().kind == Token::Kind::Minus) {
                const Op op = take().kind == Token::Kind::Plus ? Op::Add : Op::Sub;
                auto node = std::make_unique<Node>();
                node->kind = Node::Kind::BinOp; node->op = op; node->lhs = std::move(n); node->rhs = parse_term();
                n = std::move(node);
            } else break;
        }
        return n;
    }
    NodePtr parse_term() { // * /
        auto n = parse_unary();
        for (;;) {
            if (peek().kind == Token::Kind::Star || peek().kind == Token::Kind::Slash) {
                const Op op = take().kind == Token::Kind::Star ? Op::Mul : Op::Div;
                auto node = std::make_unique<Node>();
                node->kind = Node::Kind::BinOp; node->op = op; node->lhs = std::move(n); node->rhs = parse_unary();
                n = std::move(node);
            } else break;
        }
        return n;
    }
    NodePtr parse_unary() {
        if (peek().kind == Token::Kind::Minus) {
            take();
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Neg; node->lhs = parse_unary();
            return node;
        }
        return parse_primary();
    }
    NodePtr parse_primary() {
        if (peek().kind == Token::Kind::Number) {
            const double v = take().num;
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Literal; node->literal = Value{v};
            return node;
        }
        if (peek().kind == Token::Kind::String) {
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Literal; node->literal = Value{take().text};
            return node;
        }
        if (peek().kind == Token::Kind::LParen) {
            take();
            auto n = parse_comparison();
            if (peek().kind != Token::Kind::RParen) fail("expected ')'");
            take();
            return n;
        }
        if (peek().kind == Token::Kind::Ident && peek().text == "IF") return parse_if();
        if (peek().kind == Token::Kind::Ident) return parse_path_or_call();
        fail("expected a number, string, identifier or '('");
    }
    NodePtr parse_if() {
        take(); // 'IF'
        if (peek().kind != Token::Kind::LParen) fail("expected '(' after IF");
        take();
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::If;
        node->cond = parse_comparison();
        if (peek().kind != Token::Kind::Comma) fail("expected ',' after IF's condition");
        take();
        node->then_branch = parse_comparison();
        if (peek().kind != Token::Kind::Comma) fail("expected ',' after IF's then-branch");
        take();
        node->else_branch = parse_comparison();
        if (peek().kind != Token::Kind::RParen) fail("expected ')' to close IF(cond, then, else)");
        take();
        return node;
    }
    NodePtr parse_path_or_call() {
        std::vector<std::string> segs;
        segs.push_back(take().text);
        while (peek().kind == Token::Kind::Dot) {
            take();
            if (peek().kind != Token::Kind::Ident) fail("expected an identifier after '.'");
            segs.push_back(take().text);
        }
        if (peek().kind == Token::Kind::LParen) {
            if (segs.size() != 2 || segs[0] != "FUNC") fail("only FUNC.<name>(...) is callable");
            take(); // '('
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Call; node->func_name = segs[1];
            if (peek().kind != Token::Kind::RParen) {
                node->args.push_back(parse_comparison());
                while (peek().kind == Token::Kind::Comma) { take(); node->args.push_back(parse_comparison()); }
            }
            if (peek().kind != Token::Kind::RParen) fail("expected ')' to close a function call");
            take();
            return node;
        }
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Path; node->path = std::move(segs);
        return node;
    }
};

inline NodePtr parse(std::string_view expr) { return Parser(expr).parse_all(); }

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------
// User-defined functions: name -> callable, args already evaluated. A
// function returns nullopt to propagate "no value" (e.g. a bad argument),
// same convention as path resolution.
template<class Position, class Instrument>
using Func = std::function<std::optional<Value>(const std::vector<Value>&)>;
template<class Position, class Instrument>
using FuncTable = std::unordered_map<std::string, Func<Position, Instrument>>;

namespace detail {
inline std::optional<double> as_number(const Value& v) {
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
    return std::nullopt; // strings/monostate aren't arithmetic operands
}
// int64 op int64 stays int64 for + - *; / always promotes to double (integer
// division truncation is a footgun a report column shouldn't hit silently).
inline std::optional<Value> arith(Op op, const Value& a, const Value& b) {
    if (op != Op::Div) {
        if (auto* ia = std::get_if<std::int64_t>(&a))
            if (auto* ib = std::get_if<std::int64_t>(&b)) {
                switch (op) {
                    case Op::Add: return Value{*ia + *ib};
                    case Op::Sub: return Value{*ia - *ib};
                    case Op::Mul: return Value{*ia * *ib};
                    default: break;
                }
            }
    }
    const auto da = as_number(a), db = as_number(b);
    if (!da || !db) return std::nullopt;
    switch (op) {
        case Op::Add: return Value{*da + *db};
        case Op::Sub: return Value{*da - *db};
        case Op::Mul: return Value{*da * *db};
        case Op::Div: return *db == 0.0 ? std::nullopt : std::optional(Value{*da / *db});
    }
    return std::nullopt;
}

// Eq/Ne: numeric pairs compare by value (with int64/double promotion), string
// pairs by exact match, bool pairs by value. Mismatched kinds (a number vs a
// string, say) are nullopt -- not false: "not comparable" isn't "not equal".
// Lt/Le/Gt/Ge: numeric only; strings/bools have no ordering here (yet).
inline std::optional<Value> compare(CmpOp op, const Value& a, const Value& b) {
    if (op == CmpOp::Eq || op == CmpOp::Ne) {
        bool eq;
        if (auto* sa = std::get_if<std::string>(&a)) {
            auto* sb = std::get_if<std::string>(&b);
            if (!sb) return std::nullopt;
            eq = (*sa == *sb);
        } else if (auto* ba = std::get_if<bool>(&a)) {
            auto* bb = std::get_if<bool>(&b);
            if (!bb) return std::nullopt;
            eq = (*ba == *bb);
        } else {
            const auto da = as_number(a), db = as_number(b);
            if (!da || !db) return std::nullopt;
            eq = (*da == *db);
        }
        return Value{op == CmpOp::Eq ? eq : !eq};
    }
    const auto da = as_number(a), db = as_number(b);
    if (!da || !db) return std::nullopt;
    switch (op) {
        case CmpOp::Lt: return Value{*da < *db};
        case CmpOp::Le: return Value{*da <= *db};
        case CmpOp::Gt: return Value{*da > *db};
        case CmpOp::Ge: return Value{*da >= *db};
        default: return std::nullopt;
    }
}

inline bool truthy(const Value& v) {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* i = std::get_if<std::int64_t>(&v)) return *i != 0;
    if (auto* d = std::get_if<double>(&v)) return *d != 0.0;
    if (auto* s = std::get_if<std::string>(&v)) return !s->empty();
    return false; // monostate
}
} // namespace detail

template<class Position, class Instrument>
std::optional<Value> eval(const Node& n, const Position& pos, const Resolvers<Position, Instrument>& res,
                           const FuncTable<Position, Instrument>& funcs) {
    switch (n.kind) {
    case Node::Kind::Literal:
        return n.literal;
    case Node::Kind::Path: {
        std::string joined;
        for (std::size_t i = 0; i < n.path.size(); ++i) { if (i) joined += '.'; joined += n.path[i]; }
        return evaluate(joined, pos, res); // path_resolver.h
    }
    case Node::Kind::Neg: {
        const auto v = eval(*n.lhs, pos, res, funcs);
        if (!v) return std::nullopt;
        const auto d = detail::as_number(*v);
        return d ? std::optional(Value{-*d}) : std::nullopt;
    }
    case Node::Kind::BinOp: {
        const auto l = eval(*n.lhs, pos, res, funcs);
        const auto r = eval(*n.rhs, pos, res, funcs);
        if (!l || !r) return std::nullopt;
        return detail::arith(n.op, *l, *r);
    }
    case Node::Kind::Call: {
        auto it = funcs.find(n.func_name);
        if (it == funcs.end()) return std::nullopt; // unknown function: no value, not a crash
        std::vector<Value> args;
        args.reserve(n.args.size());
        for (const auto& a : n.args) {
            auto v = eval(*a, pos, res, funcs);
            if (!v) return std::nullopt; // any unresolved argument propagates
            args.push_back(*v);
        }
        return it->second(args);
    }
    case Node::Kind::Cmp: {
        const auto l = eval(*n.lhs, pos, res, funcs);
        const auto r = eval(*n.rhs, pos, res, funcs);
        if (!l || !r) return std::nullopt;
        return detail::compare(n.cmp, *l, *r);
    }
    case Node::Kind::If: {
        // Real short-circuit: only the taken branch is evaluated. This is
        // exactly why IF is a language primitive and not a FUNC -- a FUNC
        // call (Node::Kind::Call, above) evaluates every argument before the
        // function runs, which would evaluate both branches unconditionally.
        const auto c = eval(*n.cond, pos, res, funcs);
        if (!c) return std::nullopt;
        return detail::truthy(*c) ? eval(*n.then_branch, pos, res, funcs) : eval(*n.else_branch, pos, res, funcs);
    }
    }
    return std::nullopt;
}

} // namespace posreport
