#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"

namespace tree {

class DiagnosticEngine;

struct BindingId {
    uint32_t index = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != std::numeric_limits<uint32_t>::max();
    }

    friend constexpr bool operator==(BindingId, BindingId) = default;
};

enum class BindingKind : uint8_t {
    TopLevelValue,
    TopLevelFn,
    Parameter,
    Local,
};

struct BindingInfo {
    BindingKind kind{};
    std::string name;
    Span span;
    ExprId top_level_expr;
};

struct Resolution {
    std::unordered_map<uint32_t, BindingId> identifier_binding;
    std::unordered_map<uint32_t, BindingId> pattern_binding;
    std::vector<BindingInfo> bindings;
    std::unordered_map<uint32_t, std::vector<BindingId>> top_level_deps;
    std::vector<std::vector<BindingId>> binding_groups;

    [[nodiscard]] const BindingInfo& get(BindingId id) const {
        return bindings.at(id.index);
    }
};

Resolution resolve(const Program& program, DiagnosticEngine& diag);

}  // namespace tree