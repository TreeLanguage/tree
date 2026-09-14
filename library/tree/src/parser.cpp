#include "parser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "span.h"
#include "token.h"

namespace {

constexpr std::array<std::pair<tree::TokenType, tree::BinaryOp>, 2> COMPARISON_OPS = {{
    {tree::TokenType::Equal, tree::BinaryOp::Equal},
    {tree::TokenType::LessThan, tree::BinaryOp::LessThan},
}};

constexpr std::array<std::pair<tree::TokenType, tree::BinaryOp>, 2> ADDITIVE_OPS = {{
    {tree::TokenType::Plus, tree::BinaryOp::Add},
    {tree::TokenType::Minus, tree::BinaryOp::Sub},
}};

constexpr std::array<std::pair<tree::TokenType, tree::BinaryOp>, 3> MULTIPLICATIVE_OPS = {{
    {tree::TokenType::Star, tree::BinaryOp::Mul},
    {tree::TokenType::Slash, tree::BinaryOp::Div},
    {tree::TokenType::Percent, tree::BinaryOp::Mod},
}};

template <std::size_t N>
constexpr std::optional<tree::BinaryOp> lookup_op(
    const std::array<std::pair<tree::TokenType, tree::BinaryOp>, N>& table,
    tree::TokenType t) {
    for (auto& [k, v] : table) {
        if (k == t)
            return v;
    }

    return std::nullopt;
}

struct ParseAbort {};

class Parser {
public:
    Parser(std::vector<tree::Token> tokens, tree::DiagnosticEngine& diag)
        : tokens_(std::move(tokens))
        , diag_(diag) {
        prog_.arena.reserve(tokens_.size(), tokens_.size() / 4);

        if (tokens_.empty() || tokens_.back().type != tree::TokenType::Eof) {
            tokens_.push_back(make_eof_token(tokens_));
        }
    }

    tree::Program parse_program() {
        while (!check(tree::TokenType::Eof)) {
            const size_t start_pos = pos_;

            try {
                prog_.exprs.push_back(parse_top_level());
            } catch (const ParseAbort&) {
                synchronize(start_pos);
            }
        }

        merge_clauses();

        return std::move(prog_);
    }

private:
    static constexpr int K_MAX_RECURSION_DEPTH = 512;

    tree::Program prog_;
    std::vector<tree::Token> tokens_;
    tree::DiagnosticEngine& diag_;

    size_t pos_ = 0;
    int depth_ = 0;

    class DepthGuard {
    public:
        DepthGuard(Parser& p, tree::Span span)
            : p_(p) {
            if (++p_.depth_ > K_MAX_RECURSION_DEPTH) {
                --p_.depth_;
                p_.error(span, "expression nested too deeply");
            }
        }

        ~DepthGuard() {
            --p_.depth_;
        }

        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        DepthGuard(DepthGuard&&) = delete;
        DepthGuard& operator=(DepthGuard&&) = delete;

    private:
        Parser& p_;
    };

    template <typename T, typename... Args>
    tree::ExprId make_expr(tree::Span span, Args&&... args) {
        return prog_.arena.make_expr<T>(span, std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    tree::PatternId make_pattern(tree::Span span, Args&&... args) {
        return prog_.arena.make_pattern<T>(span, std::forward<Args>(args)...);
    }

    [[nodiscard]] const tree::Token& peek(size_t offset = 0) const {
        const size_t idx = pos_ + offset;

        return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
    }

    void merge_clauses() {
        std::vector<tree::ExprId> merged;
        merged.reserve(prog_.exprs.size());

        std::unordered_set<std::string> closed_names;
        std::unordered_map<std::string, tree::ExprId> group_binding;

        size_t i = 0;

        while (i < prog_.exprs.size()) {
            const std::string* name = lambda_binding_name(prog_.exprs[i]);

            if (name == nullptr) {
                merged.push_back(prog_.exprs[i]);
                ++i;
                continue;
            }

            const std::string group_name = *name;
            const tree::Span first_span = prog_.arena.get(prog_.exprs[i]).span;
            const bool reopened = closed_names.contains(group_name);

            if (reopened) {
                std::string message;
                message.reserve(64 + (group_name.size() * 2));
                message += "clauses of function '";
                message += group_name;
                message += "' must be adjacent; an earlier group for '";
                message += group_name;
                message += "' was already closed";

                diag_.report(tree::Severity::Error, first_span, std::move(message));
            }

            std::vector<tree::LambdaClause> clauses;

            tree::Span last_span = first_span;

            const tree::Span target_span =
                std::get<tree::Binding>(prog_.arena.get(prog_.exprs[i]).value).target.valid()
                    ? prog_.arena
                          .get(
                              std::get<tree::Binding>(prog_.arena.get(prog_.exprs[i]).value).target)
                          .span
                    : first_span;

            size_t j = i;

            while (j < prog_.exprs.size()) {
                const std::string* next_name = lambda_binding_name(prog_.exprs[j]);

                if (next_name == nullptr || *next_name != group_name) {
                    break;
                }

                const auto& binding =
                    std::get<tree::Binding>(prog_.arena.get(prog_.exprs[j]).value);

                const auto& lambda = std::get<tree::Lambda>(prog_.arena.get(binding.value).value);

                clauses.push_back(tree::LambdaClause{
                    .param = lambda.param,
                    .body = lambda.body,
                });

                last_span = prog_.arena.get(prog_.exprs[j]).span;
                ++j;
            }

            closed_names.insert(group_name);

            if (reopened) {
                splice_into_existing_group(group_binding.at(group_name),
                                           std::move(clauses),
                                           last_span);

                i = j;
                continue;
            }

            if (clauses.size() == 1) {
                merged.push_back(prog_.exprs[i]);
                group_binding.emplace(group_name, prog_.exprs[i]);
                i = j;
                continue;
            }

            const tree::Span span{first_span.begin, last_span.end};

            const tree::ExprId multi = prog_.arena.make_expr<tree::MultiClauseLambda>(
                span,
                std::optional<std::string>(group_name),
                std::move(clauses));

            const tree::PatternId lhs =
                prog_.arena.make_pattern<tree::VarPattern>(target_span, group_name);

            const tree::ExprId binding_id = prog_.arena.make_expr<tree::Binding>(span, lhs, multi);

            merged.push_back(binding_id);
            group_binding.emplace(group_name, binding_id);

            i = j;
        }

        prog_.exprs = std::move(merged);
    }

    void splice_into_existing_group(tree::ExprId existing_binding_id,
                                    std::vector<tree::LambdaClause> new_clauses,
                                    tree::Span new_end_span) {
        tree::Expr& binding_expr = prog_.arena.get(existing_binding_id);
        binding_expr.span = tree::Span(binding_expr.span.begin, new_end_span.end);

        auto& binding = std::get<tree::Binding>(binding_expr.value);
        tree::Expr& value_expr = prog_.arena.get(binding.value);

        if (auto* multi = std::get_if<tree::MultiClauseLambda>(&value_expr.value)) {
            value_expr.span = tree::Span(value_expr.span.begin, new_end_span.end);
            multi->clauses.insert(multi->clauses.end(),
                                  std::make_move_iterator(new_clauses.begin()),
                                  std::make_move_iterator(new_clauses.end()));

            return;
        }

        const auto& lambda = std::get<tree::Lambda>(value_expr.value);

        std::vector<tree::LambdaClause> clauses;
        clauses.reserve(1 + new_clauses.size());
        clauses.push_back(tree::LambdaClause{.param = lambda.param, .body = lambda.body});

        for (auto& clause : new_clauses) {
            clauses.push_back(std::move(clause));
        }

        const tree::Span span{value_expr.span.begin, new_end_span.end};
        const std::optional<std::string> name = lambda.name;

        const tree::ExprId new_value_id =
            prog_.arena.make_expr<tree::MultiClauseLambda>(span, name, std::move(clauses));

        std::get<tree::Binding>(prog_.arena.get(existing_binding_id).value).value = new_value_id;
    }

    [[nodiscard]] const std::string* lambda_binding_name(tree::ExprId id) const {
        const auto* binding = std::get_if<tree::Binding>(&prog_.arena.get(id).value);

        if (binding == nullptr) {
            return nullptr;
        }

        const auto* var = std::get_if<tree::VarPattern>(&prog_.arena.get(binding->target).value);

        if (var == nullptr) {
            return nullptr;
        }

        const auto* lambda = std::get_if<tree::Lambda>(&prog_.arena.get(binding->value).value);

        if (lambda == nullptr || !lambda->name.has_value() || *lambda->name != var->name) {
            return nullptr;
        }

        return &var->name;
    }

    static tree::Token make_eof_token(const std::vector<tree::Token>& tokens) {
        const tree::Position pos = tokens.empty() ? tree::Position{1, 1} : tokens.back().span.end;

        return tree::Token{
            .type = tree::TokenType::Eof,
            .span = tree::Span(pos, pos),
            .string_value = {},
        };
    }

    const tree::Token& advance() {
        const tree::Token& t = peek();

        if (pos_ + 1 < tokens_.size()) {
            pos_++;
        }

        return t;
    }

    [[nodiscard]] bool check(tree::TokenType type) const {
        return peek().type == type;
    }

    bool match(tree::TokenType type) {
        if (!check(type)) {
            return false;
        }

        advance();

        return true;
    }

    [[noreturn]] void error(tree::Span span, std::string message) {
        diag_.report(tree::Severity::Error, span, std::move(message));
        throw ParseAbort{};
    }

    const tree::Token& expect(tree::TokenType type, const char* what) {
        if (!check(type)) {
            error(peek().span,
                  std::string("expected ") + what + " but found " +
                      std::string(tree::to_string(peek().type)));
        }

        return advance();
    }

    const tree::Token& expect_close(tree::TokenType type,
                                    const char* what,
                                    tree::Span open_span,
                                    const char* open_what) {
        if (!check(type)) {
            const tree::Span span{open_span.begin, peek().span.end};

            error(span,
                  std::string("unclosed ") + open_what + ": expected " + what + " but found " +
                      std::string(tree::to_string(peek().type)));
        }

        return advance();
    }

    void synchronize(size_t failed_at) {
        if (pos_ <= failed_at) {
            advance();
        }

        while (!check(tree::TokenType::Eof)) {
            if (check(tree::TokenType::Identifier)) {
                return;
            }

            advance();
        }
    }

    tree::PatternId expr_to_pattern(tree::ExprId expr_id) {
        const DepthGuard guard(*this, prog_.arena.get(expr_id).span);

        const tree::Expr& expr = prog_.arena.get(expr_id);
        const tree::Span span = expr.span;

        if (const auto* lit = std::get_if<tree::FloatLiteral>(&expr.value)) {
            return make_pattern<tree::FloatPattern>(span, lit->value);
        }

        if (const auto* lit = std::get_if<tree::StringLiteral>(&expr.value)) {
            return make_pattern<tree::StringPattern>(span, lit->value);
        }

        if (const auto* lit = std::get_if<tree::BoolLiteral>(&expr.value)) {
            return make_pattern<tree::BoolPattern>(span, lit->value);
        }

        if (const auto* id = std::get_if<tree::Identifier>(&expr.value)) {
            if (id->name == "_") {
                return make_pattern<tree::WildcardPattern>(span);
            }

            return make_pattern<tree::VarPattern>(span, id->name);
        }

        if (const auto* tup = std::get_if<tree::TupleExpr>(&expr.value)) {
            std::vector<tree::TuplePatternField> fields;
            fields.reserve(tup->fields.size());

            std::ranges::transform(tup->fields, std::back_inserter(fields), [this](auto& f) {
                return tree::TuplePatternField{
                    f.name,
                    expr_to_pattern(f.value),
                };
            });

            return make_pattern<tree::TuplePattern>(span, std::move(fields));
        }

        error(span, "invalid pattern");
    }

    tree::ExprId parse_top_level() {
        const tree::Span start = peek().span;
        const tree::ExprId head = parse_unary();

        if (!match(tree::TokenType::Assign)) {
            tree::ExprId expr =
                continue_binary_level(MULTIPLICATIVE_OPS, &Parser::parse_unary, head);

            expr = continue_binary_level(ADDITIVE_OPS, &Parser::parse_multiplicative, expr);

            expr = continue_binary_level(COMPARISON_OPS, &Parser::parse_additive, expr);

            return expr;
        }

        const tree::ExprId body = parse_expr();
        const tree::Span span{start.begin, prog_.arena.get(body).span.end};
        const tree::Expr& head_expr = prog_.arena.get(head);

        if (const auto* call = std::get_if<tree::Call>(&head_expr.value)) {
            const tree::Expr& callee_expr = prog_.arena.get(call->callee);

            if (const auto* callee_id = std::get_if<tree::Identifier>(&callee_expr.value)) {
                std::string name = callee_id->name;
                const tree::Span callee_span = callee_expr.span;
                const tree::PatternId param = expr_to_pattern(call->arg);
                const tree::ExprId lambda = make_expr<tree::Lambda>(span, name, param, body);

                const tree::PatternId lhs =
                    make_pattern<tree::VarPattern>(callee_span, std::move(name));

                return make_expr<tree::Binding>(span, lhs, lambda);
            }
        }

        return make_expr<tree::Binding>(span, expr_to_pattern(head), body);
    }

    tree::ExprId parse_expr() {
        return parse_comparison();
    }

    tree::ExprId parse_comparison() {
        return parse_binary_level(COMPARISON_OPS, &Parser::parse_additive);
    }

    tree::ExprId parse_additive() {
        return parse_binary_level(ADDITIVE_OPS, &Parser::parse_multiplicative);
    }

    tree::ExprId parse_multiplicative() {
        return parse_binary_level(MULTIPLICATIVE_OPS, &Parser::parse_unary);
    }

    template <std::size_t N>
    tree::ExprId continue_binary_level(
        const std::array<std::pair<tree::TokenType, tree::BinaryOp>, N>& ops,
        tree::ExprId (Parser::*next)(),
        tree::ExprId lhs) {
        for (;;) {
            auto op = lookup_op(ops, peek().type);

            if (!op) {
                break;
            }

            advance();

            const tree::ExprId rhs = (this->*next)();
            const tree::Span span{
                prog_.arena.get(lhs).span.begin,
                prog_.arena.get(rhs).span.end,
            };

            lhs = make_expr<tree::BinaryExpr>(span, *op, lhs, rhs);
        }

        return lhs;
    }

    template <std::size_t N>
    tree::ExprId parse_binary_level(
        const std::array<std::pair<tree::TokenType, tree::BinaryOp>, N>& ops,
        tree::ExprId (Parser::*next)()) {
        const tree::ExprId lhs = (this->*next)();

        return continue_binary_level(ops, next, lhs);
    }

    tree::ExprId parse_unary() {
        const DepthGuard guard(*this, peek().span);

        if (check(tree::TokenType::Minus)) {
            const tree::Span start = peek().span;
            advance();

            const tree::ExprId operand = parse_unary();
            const tree::Span span{
                start.begin,
                prog_.arena.get(operand).span.end,
            };

            const tree::ExprId zero = make_expr<tree::FloatLiteral>(start, 0.0);

            return make_expr<tree::BinaryExpr>(span, tree::BinaryOp::Sub, zero, operand);
        }

        return parse_postfix();
    }

    tree::ExprId parse_postfix() {
        tree::ExprId expr = parse_primary();

        for (;;) {
            if (check(tree::TokenType::LeftParen) &&
                peek().span.begin.line == prog_.arena.get(expr).span.end.line) {
                expr = parse_call(expr);
            } else {
                break;
            }
        }

        return expr;
    }

    tree::ExprId parse_call(tree::ExprId callee) {
        const tree::Span start = prog_.arena.get(callee).span;
        const tree::Token open = expect(tree::TokenType::LeftParen, "'('");

        if (check(tree::TokenType::RightParen)) {
            error(peek().span, "function calls require exactly one parameter");
        }

        const tree::ExprId arg = parse_unary();

        if (check(tree::TokenType::Comma)) {
            error(peek().span, "function calls require exactly one parameter");
        }

        const tree::Token close =
            expect_close(tree::TokenType::RightParen, "')'", open.span, "'('");

        const tree::Span span{start.begin, close.span.end};

        return make_expr<tree::Call>(span, callee, arg);
    }

    tree::ExprId parse_primary() {
        const tree::Token& t = peek();

        switch (t.type) {
            case tree::TokenType::Float:
                advance();
                return make_expr<tree::FloatLiteral>(t.span, t.float_value);

            case tree::TokenType::String:
                advance();
                return make_expr<tree::StringLiteral>(t.span, t.string_value);

            case tree::TokenType::Bool:
                advance();
                return make_expr<tree::BoolLiteral>(t.span, t.string_value == "true");

            case tree::TokenType::Identifier:
                advance();
                return make_expr<tree::Identifier>(t.span, t.string_value);

            case tree::TokenType::LeftParen:
                return parse_tuple_or_paren();

            case tree::TokenType::Backslash:
                return parse_lambda();

            default:
                error(t.span, "unexpected token " + std::string(tree::to_string(t.type)));
        }
    }

    tree::ExprId parse_tuple_or_paren() {
        const tree::Span start = peek().span;
        const tree::Token open = expect(tree::TokenType::LeftParen, "'('");

        if (check(tree::TokenType::RightParen)) {
            const tree::Token close = advance();
            const tree::Span span{start.begin, close.span.end};

            return make_expr<tree::TupleExpr>(span, tree::TupleExpr{});
        }

        std::vector<tree::TupleExprField> fields;
        fields.push_back(parse_tuple_field());

        bool saw_comma = false;

        while (match(tree::TokenType::Comma)) {
            saw_comma = true;
            fields.push_back(parse_tuple_field());
        }

        const tree::Token close =
            expect_close(tree::TokenType::RightParen, "')'", open.span, "'('");

        const tree::Span span{start.begin, close.span.end};

        if (!saw_comma && fields.size() == 1 && !fields[0].name) {
            return std::move(fields[0].value);
        }

        return make_expr<tree::TupleExpr>(span, std::move(fields));
    }

    tree::TupleExprField parse_tuple_field() {
        if (check(tree::TokenType::Identifier) && peek(1).type == tree::TokenType::Colon) {
            std::string name = advance().string_value;
            advance();

            tree::ExprId value = parse_expr();

            return tree::TupleExprField{
                .name = std::optional<std::string>(std::move(name)),
                .value = std::move(value),
            };
        }

        return tree::TupleExprField{
            .name = std::nullopt,
            .value = parse_expr(),
        };
    }

    tree::ExprId parse_lambda() {
        const tree::Span start = peek().span;

        expect(tree::TokenType::Backslash, "'\\'");

        const tree::PatternId param = expr_to_pattern(parse_postfix());

        expect(tree::TokenType::Arrow, "'->'");

        const tree::ExprId body = parse_expr();
        const tree::Span span{
            start.begin,
            prog_.arena.get(body).span.end,
        };

        return make_expr<tree::Lambda>(span, std::nullopt, param, body);
    }
};

}  // namespace

namespace tree {

Program parse(std::vector<Token> tokens, DiagnosticEngine& diag) {
    Parser parser(std::move(tokens), diag);

    return parser.parse_program();
}

}  // namespace tree