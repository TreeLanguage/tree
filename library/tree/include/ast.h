#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "span.h"

namespace tree {

struct PatternId {
    uint32_t index = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != std::numeric_limits<uint32_t>::max();
    }

    friend constexpr bool operator==(PatternId, PatternId) = default;
};

struct ExprId {
    uint32_t index = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != std::numeric_limits<uint32_t>::max();
    }

    friend bool operator==(ExprId, ExprId) = default;
};

struct WildcardPattern {};

struct VarPattern {
    std::string name;
};

struct FloatPattern {
    double value;
};

struct StringPattern {
    std::string value;
};

struct BoolPattern {
    bool value;
};

struct TuplePatternField {
    std::optional<std::string> name;
    PatternId pattern;
};

struct TuplePattern {
    std::vector<TuplePatternField> fields;
};

struct Pattern {
    std::
        variant<WildcardPattern, VarPattern, FloatPattern, StringPattern, BoolPattern, TuplePattern>
            value;
    Span span;

    template <typename T, typename... Args>
    Pattern(std::in_place_type_t<T> /*unused*/, Span new_span, Args&&... args)
        : value(std::in_place_type<T>, std::forward<Args>(args)...)
        , span(new_span) {}
};

struct FloatLiteral {
    double value;
};

struct StringLiteral {
    std::string value;
};

struct BoolLiteral {
    bool value;
};

struct Identifier {
    std::string name;
};

struct TupleExprField {
    std::optional<std::string> name;
    ExprId value;
};

struct TupleExpr {
    std::vector<TupleExprField> fields;
};

struct Call {
    ExprId callee;
    ExprId arg;
};

enum class BinaryOp : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Equal,
    LessThan
};

struct BinaryExpr {
    BinaryOp op{};
    ExprId lhs;
    ExprId rhs;
};

struct Lambda {
    std::optional<std::string> name;
    PatternId param;
    ExprId body;
};

struct LambdaClause {
    PatternId param;
    ExprId body;
};

struct MultiClauseLambda {
    std::optional<std::string> name;
    std::vector<LambdaClause> clauses;
};

struct Binding {
    PatternId target;
    ExprId value;
};

struct Expr {
    std::variant<FloatLiteral,
                 StringLiteral,
                 BoolLiteral,
                 Identifier,
                 TupleExpr,
                 Call,
                 BinaryExpr,
                 Lambda,
                 Binding,
                 MultiClauseLambda>
        value;
    Span span;

    template <typename T, typename... Args>
    Expr(std::in_place_type_t<T> /*unused*/, Span new_span, Args&&... args)
        : value(std::in_place_type<T>, std::forward<Args>(args)...)
        , span(new_span) {}
};

class Arena {
public:
    template <typename T, typename... Args>
    ExprId make_expr(Span span, Args&&... args) {
        exprs_.emplace_back(std::in_place_type<T>, span, std::forward<Args>(args)...);

        return ExprId{static_cast<uint32_t>(exprs_.size() - 1)};
    }

    template <typename T, typename... Args>
    PatternId make_pattern(Span span, Args&&... args) {
        patterns_.emplace_back(std::in_place_type<T>, span, std::forward<Args>(args)...);

        return PatternId{static_cast<uint32_t>(patterns_.size() - 1)};
    }

    [[nodiscard]] Expr& get(ExprId id) {
        assert(id.index < exprs_.size() && "Arena::get: invalid or out-of-range ExprId");

        return exprs_[id.index];
    }

    [[nodiscard]] const Expr& get(ExprId id) const {
        assert(id.index < exprs_.size() && "Arena::get: invalid or out-of-range ExprId");

        return exprs_[id.index];
    }

    [[nodiscard]] Pattern& get(PatternId id) {
        assert(id.index < patterns_.size() && "Arena::get: invalid or out-of-range PatternId");

        return patterns_[id.index];
    }

    [[nodiscard]] const Pattern& get(PatternId id) const {
        assert(id.index < patterns_.size() && "Arena::get: invalid or out-of-range PatternId");

        return patterns_[id.index];
    }

    [[nodiscard]] size_t expr_count() const {
        return exprs_.size();
    }

    [[nodiscard]] size_t pattern_count() const {
        return patterns_.size();
    }

    void reserve(size_t expr_hint, size_t pattern_hint) {
        exprs_.reserve(expr_hint);
        patterns_.reserve(pattern_hint);
    }

    ExprId clone(const Arena& src, ExprId id);
    PatternId clone(const Arena& src, PatternId id);

private:
    std::vector<Expr> exprs_;
    std::vector<Pattern> patterns_;
};

struct Program {
    Arena arena;
    std::vector<ExprId> exprs;
};

Program clone(const Program& prog);
std::string to_string(const Arena& arena, ExprId id);
std::string to_string(const Arena& arena, PatternId id);
std::string to_string(const Program& prog);

template <typename... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};

template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

template <typename Variant, typename... Fs>
decltype(auto) match(Variant&& v, Fs&&... fs) {
    return std::visit(Overloaded{std::forward<Fs>(fs)...}, std::forward<Variant>(v));
}

}  // namespace tree