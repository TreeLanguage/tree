#include "resolver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "span.h"

namespace {
struct Scope {
    std::unordered_map<std::string, tree::BindingId> locals;
    Scope* parent = nullptr;

    [[nodiscard]] std::optional<tree::BindingId> lookup(const std::string& name) const {
        for (const Scope* scope = this; scope != nullptr; scope = scope->parent) {
            const auto it = scope->locals.find(name);
            if (it != scope->locals.end()) {
                return it->second;
            }
        }

        return std::nullopt;
    }
};

class Resolver final {
public:
    Resolver(const tree::Program& program, tree::DiagnosticEngine& diag)
        : program_(program)
        , arena_(program.arena)
        , diag_(diag) {}

    tree::Resolution run() {
        collect_top_level_functions();
        resolve_top_level_declarations();
        compute_binding_groups();

        return std::move(resolution_);
    }

private:
    const tree::Program& program_;
    const tree::Arena& arena_;
    tree::DiagnosticEngine& diag_;
    tree::Resolution resolution_;
    std::unordered_map<std::string, tree::BindingId> top_level_;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> dependency_sets_;
    std::unordered_map<uint32_t, tree::BindingId> top_level_expr_bindings_;
    std::unordered_map<uint32_t, tree::BindingId> pending_pattern_bindings_;

    tree::BindingId add_binding(tree::BindingKind kind,
                                std::string name,
                                tree::Span span,
                                tree::ExprId top_level_expr = {}) {
        if (resolution_.bindings.size() >=
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::length_error("resolution_.bindings.size() exceeds uint32_t range");
        }
        const tree::BindingId id{static_cast<uint32_t>(resolution_.bindings.size())};
        resolution_.bindings.push_back(tree::BindingInfo{.kind = kind,
                                                         .name = std::move(name),
                                                         .span = span,
                                                         .top_level_expr = top_level_expr});

        return id;
    }

    [[nodiscard]] bool is_function_value(tree::ExprId id) const {
        const tree::Expr& expr = arena_.get(id);

        return std::holds_alternative<tree::Lambda>(expr.value) ||
               std::holds_alternative<tree::MultiClauseLambda>(expr.value);
    }

    [[nodiscard]] std::optional<std::string> function_name(tree::ExprId id) const {
        const tree::Expr& expr = arena_.get(id);
        const auto* binding = std::get_if<tree::Binding>(&expr.value);
        if (binding == nullptr || !is_function_value(binding->value)) {
            return std::nullopt;
        }
        const tree::Pattern& target = arena_.get(binding->target);
        const auto* variable = std::get_if<tree::VarPattern>(&target.value);
        if (variable == nullptr) {
            return std::nullopt;
        }
        const tree::Expr& value = arena_.get(binding->value);

        return tree::match(
            value.value,
            [&](const tree::Lambda& lambda) -> std::optional<std::string> {
                if (!lambda.name || *lambda.name != variable->name) {
                    return std::nullopt;
                }

                return variable->name;
            },
            [&](const tree::MultiClauseLambda& lambda) -> std::optional<std::string> {
                if (!lambda.name || *lambda.name != variable->name) {
                    return std::nullopt;
                }

                return variable->name;
            },
            [&](const auto&) -> std::optional<std::string> {
                return std::nullopt;
            });
    }

    void collect_top_level_functions() {
        for (const tree::ExprId expr_id : program_.exprs) {
            const auto name = function_name(expr_id);
            if (!name) {
                continue;
            }
            const tree::Expr& expr = arena_.get(expr_id);
            const auto& binding = std::get<tree::Binding>(expr.value);
            const tree::Pattern& target = arena_.get(binding.target);
            register_function(*name, target.span, expr_id);
        }
    }

    void register_function(const std::string& name, tree::Span span, tree::ExprId owner_expr) {
        if (top_level_.contains(name)) {
            diag_.report(tree::Severity::Error,
                         span,
                         "duplicate top-level function '" + name + "'");

            return;
        }
        const tree::BindingId id =
            add_binding(tree::BindingKind::TopLevelFn, name, span, owner_expr);
        top_level_.emplace(name, id);
        top_level_expr_bindings_.emplace(owner_expr.index, id);
    }

    void resolve_top_level_declarations() {
        for (const tree::ExprId expr_id : program_.exprs) {
            const tree::Expr& expr = arena_.get(expr_id);
            tree::match(
                expr.value,
                [&](const tree::Binding& binding) {
                    resolve_top_level_binding(expr_id, binding);
                },
                [&](const tree::Lambda&) {
                    resolve_top_level_expression(expr_id);
                },
                [&](const tree::MultiClauseLambda&) {
                    resolve_top_level_expression(expr_id);
                },
                [&](const auto&) {
                    resolve_top_level_expression(expr_id);
                });
        }
    }

    void resolve_top_level_binding(tree::ExprId owner_expr, const tree::Binding& binding) {
        if (const auto name = function_name(owner_expr)) {
            resolve_function_binding(owner_expr, binding);

            return;
        }
        const std::vector<tree::BindingId> owners =
            prepare_value_bindings(owner_expr, binding.target);
        const std::span<const tree::BindingId> context(owners.data(), owners.size());
        Scope root;
        resolve_expr(binding.value, root, context);
        commit_value_bindings(binding.target);
    }

    void resolve_function_binding(tree::ExprId owner_expr, const tree::Binding& binding) {
        const auto it = top_level_expr_bindings_.find(owner_expr.index);
        if (it == top_level_expr_bindings_.end()) {
            Scope root;
            resolve_expr(binding.value, root, {});

            return;
        }
        const tree::BindingId owner = it->second;
        Scope root;
        resolve_expr(binding.value, root, std::span<const tree::BindingId>(&owner, 1));
    }

    void resolve_top_level_expression(tree::ExprId expr_id) {
        Scope root;
        resolve_expr(expr_id, root, {});
    }

    std::vector<tree::BindingId> prepare_value_bindings(tree::ExprId owner_expr,
                                                        tree::PatternId pattern_id) {
        pending_pattern_bindings_.clear();
        std::vector<tree::BindingId> owners;
        std::unordered_set<std::string> names;
        collect_value_bindings(owner_expr, pattern_id, names, owners);

        return owners;
    }

    void collect_value_bindings(tree::ExprId owner_expr,
                                tree::PatternId pattern_id,
                                std::unordered_set<std::string>& names,
                                std::vector<tree::BindingId>& owners) {
        const tree::Pattern& pattern = arena_.get(pattern_id);
        tree::match(
            pattern.value,
            [&](const tree::VarPattern& variable) {
                if (!names.insert(variable.name).second) {
                    diag_.report(tree::Severity::Error,
                                 pattern.span,
                                 "duplicate binder '" + variable.name + "' in pattern");

                    return;
                }
                const tree::BindingId id = add_binding(tree::BindingKind::TopLevelValue,
                                                       variable.name,
                                                       pattern.span,
                                                       owner_expr);
                owners.push_back(id);
                pending_pattern_bindings_.emplace(pattern_id.index, id);
            },
            [&](const tree::TuplePattern& tuple) {
                for (const auto& field : tuple.fields) {
                    collect_value_bindings(owner_expr, field.pattern, names, owners);
                }
            },
            [&](const auto&) {});
    }

    void commit_value_bindings(tree::PatternId pattern_id) {
        commit_value_pattern(pattern_id);
        pending_pattern_bindings_.clear();
    }

    void commit_value_pattern(tree::PatternId pattern_id) {
        const tree::Pattern& pattern = arena_.get(pattern_id);
        tree::match(
            pattern.value,
            [&](const tree::VarPattern&) {
                const auto it = pending_pattern_bindings_.find(pattern_id.index);
                if (it == pending_pattern_bindings_.end()) {
                    return;
                }
                const tree::BindingId id = it->second;
                const std::string& name = resolution_.bindings[id.index].name;
                top_level_[name] = id;
                resolution_.pattern_binding[pattern_id.index] = id;
            },
            [&](const tree::TuplePattern& tuple) {
                for (const auto& field : tuple.fields) {
                    commit_value_pattern(field.pattern);
                }
            },
            [&](const auto&) {});
    }

    void bind_local_pattern(tree::PatternId pattern_id,
                            Scope& scope,
                            std::unordered_set<std::string>* seen = nullptr) {
        std::unordered_set<std::string> local_seen;
        if (seen == nullptr) {
            seen = &local_seen;
        }
        const tree::Pattern& pattern = arena_.get(pattern_id);
        tree::match(
            pattern.value,
            [&](const tree::VarPattern& variable) {
                if (!seen->insert(variable.name).second) {
                    diag_.report(tree::Severity::Error,
                                 pattern.span,
                                 "duplicate binder '" + variable.name + "' in pattern");

                    return;
                }
                const tree::BindingId id =
                    add_binding(tree::BindingKind::Local, variable.name, pattern.span);
                scope.locals[variable.name] = id;
                resolution_.pattern_binding[pattern_id.index] = id;
            },
            [&](const tree::TuplePattern& tuple) {
                for (const auto& field : tuple.fields) {
                    bind_local_pattern(field.pattern, scope, seen);
                }
            },
            [&](const auto&) {});
    }

    void resolve_expr(tree::ExprId expr_id,
                      Scope& scope,
                      std::span<const tree::BindingId> top_level_owners) {
        const tree::Expr& expr = arena_.get(expr_id);
        tree::match(
            expr.value,
            [&](const tree::Identifier& identifier) {
                resolve_identifier(expr_id, identifier, expr.span, scope, top_level_owners);
            },
            [&](const tree::TupleExpr& tuple) {
                for (const auto& field : tuple.fields) {
                    resolve_expr(field.value, scope, top_level_owners);
                }
            },
            [&](const tree::Call& call) {
                resolve_expr(call.callee, scope, top_level_owners);
                resolve_expr(call.arg, scope, top_level_owners);
            },
            [&](const tree::BinaryExpr& binary) {
                resolve_expr(binary.lhs, scope, top_level_owners);
                resolve_expr(binary.rhs, scope, top_level_owners);
            },
            [&](const tree::Lambda& lambda) {
                resolve_lambda_like(lambda, scope, top_level_owners);
            },
            [&](const tree::MultiClauseLambda& lambda) {
                resolve_multi_clause(lambda, scope, top_level_owners);
            },
            [&](const auto&) {});
    }

    void resolve_lambda_like(const tree::Lambda& lambda,
                             Scope& parent,
                             std::span<const tree::BindingId> top_level_owners) {
        Scope function_scope{.locals = {}, .parent = &parent};
        bind_local_pattern(lambda.param, function_scope);
        resolve_expr(lambda.body, function_scope, top_level_owners);
    }

    void resolve_multi_clause(const tree::MultiClauseLambda& lambda,
                              Scope& parent,
                              std::span<const tree::BindingId> top_level_owners) {
        for (const auto& clause : lambda.clauses) {
            Scope clause_scope{.locals = {}, .parent = &parent};
            bind_local_pattern(clause.param, clause_scope);
            resolve_expr(clause.body, clause_scope, top_level_owners);
        }
    }

    void resolve_identifier(tree::ExprId expr_id,
                            const tree::Identifier& identifier,
                            tree::Span span,
                            Scope& scope,
                            std::span<const tree::BindingId> top_level_owners) {
        if (const auto local = scope.lookup(identifier.name)) {
            resolution_.identifier_binding[expr_id.index] = *local;

            return;
        }
        const auto top_level = top_level_.find(identifier.name);
        if (top_level == top_level_.end()) {
            diag_.report(tree::Severity::Error,
                         span,
                         "undefined identifier '" + identifier.name + "'");

            return;
        }
        const tree::BindingId binding = top_level->second;
        resolution_.identifier_binding[expr_id.index] = binding;
        for (const tree::BindingId owner : top_level_owners) {
            dependency_sets_[owner.index].insert(binding.index);
        }
    }

    void materialize_dependency_sets() {
        resolution_.top_level_deps.clear();
        for (auto& [owner, dependencies] : dependency_sets_) {
            std::vector<tree::BindingId> ids;
            ids.reserve(dependencies.size());
            std::ranges::transform(dependencies, std::back_inserter(ids), [](uint32_t dependency) {
                return tree::BindingId{dependency};
            });
            std::ranges::sort(ids, [](tree::BindingId lhs, tree::BindingId rhs) {
                return lhs.index < rhs.index;
            });
            resolution_.top_level_deps.emplace(owner, std::move(ids));
        }
    }

    std::vector<std::vector<tree::BindingId>> compute_strongly_connected_components() {
        const size_t binding_count = resolution_.bindings.size();
        std::vector<int32_t> index(binding_count, -1);
        std::vector<int32_t> lowlink(binding_count, -1);
        std::vector<bool> on_stack(binding_count, false);
        std::vector<tree::BindingId> stack;
        std::vector<std::vector<tree::BindingId>> components;
        int32_t next_index = 0;

        std::function<void(tree::BindingId)> strongconnect = [&](tree::BindingId vertex) {
            index[vertex.index] = next_index;
            lowlink[vertex.index] = next_index;
            ++next_index;
            stack.push_back(vertex);
            on_stack[vertex.index] = true;

            const auto it = resolution_.top_level_deps.find(vertex.index);
            if (it != resolution_.top_level_deps.end()) {
                for (const tree::BindingId dependency : it->second) {
                    if (resolution_.bindings[dependency.index].kind == tree::BindingKind::Local) {
                        continue;
                    }
                    if (index[dependency.index] == -1) {
                        strongconnect(dependency);
                        lowlink[vertex.index] =
                            std::min(lowlink[vertex.index], lowlink[dependency.index]);
                    } else if (on_stack[dependency.index]) {
                        lowlink[vertex.index] =
                            std::min(lowlink[vertex.index], index[dependency.index]);
                    }
                }
            }

            if (lowlink[vertex.index] != index[vertex.index]) {
                return;
            }
            std::vector<tree::BindingId> component;
            while (true) {
                const tree::BindingId member = stack.back();
                stack.pop_back();
                on_stack[member.index] = false;
                component.push_back(member);
                if (component.back() == vertex) {
                    break;
                }
            }
            std::ranges::sort(component, [](tree::BindingId lhs, tree::BindingId rhs) {
                return lhs.index < rhs.index;
            });
            components.push_back(std::move(component));
        };

        for (uint32_t i = 0; i < binding_count; ++i) {
            if (resolution_.bindings[i].kind == tree::BindingKind::Local || index[i] != -1) {
                continue;
            }
            strongconnect(tree::BindingId{i});
        }
        return components;
    }

    void build_component_dependency_graph(
        const std::vector<std::vector<tree::BindingId>>& components,
        std::vector<std::unordered_set<size_t>>& dependents,
        std::vector<size_t>& indegree) {
        std::vector<size_t> component_of(resolution_.bindings.size(), SIZE_MAX);
        for (size_t component = 0; component < components.size(); ++component) {
            for (const tree::BindingId binding : components[component]) {
                component_of[binding.index] = component;
            }
        }

        dependents.assign(components.size(), {});
        indegree.assign(components.size(), 0);
        for (size_t component = 0; component < components.size(); ++component) {
            for (const tree::BindingId binding : components[component]) {
                const auto it = resolution_.top_level_deps.find(binding.index);
                if (it == resolution_.top_level_deps.end()) {
                    continue;
                }
                for (const tree::BindingId dependency : it->second) {
                    const size_t dependency_component = component_of[dependency.index];
                    if (dependency_component == component) {
                        continue;
                    }
                    if (dependents[dependency_component].insert(component).second) {
                        ++indegree[component];
                    }
                }
            }
        }
    }

    static std::vector<std::vector<tree::BindingId>> topological_sort_components(
        const std::vector<std::vector<tree::BindingId>>& components,
        const std::vector<std::unordered_set<size_t>>& dependents,
        std::vector<size_t> indegree) {
        const auto component_key = [&](size_t component) {
            return components[component].front().index;
        };

        std::vector<size_t> ready;
        auto indices = std::ranges::views::iota(size_t{0}, components.size());
        std::ranges::copy_if(indices, std::back_inserter(ready), [&](size_t component) {
            return indegree[component] == 0;
        });
        std::ranges::sort(ready, [&](size_t lhs, size_t rhs) {
            return component_key(lhs) < component_key(rhs);
        });

        std::vector<std::vector<tree::BindingId>> ordered;
        ordered.reserve(components.size());
        while (!ready.empty()) {
            const size_t component = ready.front();
            ready.erase(ready.begin());
            ordered.push_back(components[component]);
            for (const size_t dependent : dependents[component]) {
                if (indegree[dependent] == 0) {
                    throw std::logic_error("indegree underflow in topological sort");
                }
                if (--indegree[dependent] == 0) {
                    ready.push_back(dependent);
                }
            }
            std::ranges::sort(ready, [&](size_t lhs, size_t rhs) {
                return component_key(lhs) < component_key(rhs);
            });
        }
        return ordered;
    }

    void compute_binding_groups() {
        materialize_dependency_sets();
        const auto components = compute_strongly_connected_components();

        std::vector<std::unordered_set<size_t>> dependents;
        std::vector<size_t> indegree;
        build_component_dependency_graph(components, dependents, indegree);

        resolution_.binding_groups =
            topological_sort_components(components, dependents, std::move(indegree));
    }
};
}  // namespace

namespace tree {
Resolution resolve(const Program& program, DiagnosticEngine& diag) {
    Resolver resolver(program, diag);

    return resolver.run();
}
}  // namespace tree