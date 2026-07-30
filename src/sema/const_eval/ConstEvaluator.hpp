/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sema {

/// @brief Evaluates const declarations at compile-time.
///
/// This class walks the AST, identifies const declarations, and evaluates
/// their initializers at compile-time. Results are stored on the expression
/// nodes themselves (ExprAST::constValue).
///
/// @design_decision Expressions are mutable, declarations are immutable
///   Since ExprAST nodes can be modified (resolvedType, valueState, etc.),
///   we store the evaluated constant value directly on the expression.
///   DeclAST nodes remain immutable.
class ConstEvaluator {
public:
    explicit ConstEvaluator(SemaContext& ctx);

    /// @brief Main entry point: evaluate all const declarations.
    void evaluateAll();

    /// @brief Evaluate a specific const declaration.
    /// @return The evaluated value, or ConstantValue::error() on failure.
    ConstantValue evaluateDecl(const DeclAST* decl);

    /// @brief Check if an expression has been evaluated.
    bool isEvaluated(const ExprAST* expr) const;

    /// @brief Get the evaluated value of an expression.
    /// @return The constant value, or ConstantValue::unknown() if not evaluated.
    ConstantValue getValue(const ExprAST* expr) const;

    /// @brief Get the SemaContext.
    SemaContext& context() const { return const_cast<SemaContext&>(m_ctx); }

    // ─── Expression Evaluation ───────────────────────────────────────

    /// @brief Evaluate an expression and store result on the expression.
    /// @param expr The expression to evaluate.
    /// @return The evaluated constant value.
    ConstantValue evalExpr(const ExprAST* expr);

    /// @brief Evaluate a literal expression.
    ConstantValue evalLiteral(const LiteralExprAST* expr);

    /// @brief Evaluate an identifier expression.
    ConstantValue evalIdentifier(const IdentifierExprAST* expr);

    /// @brief Evaluate a binary expression.
    ConstantValue evalBinary(const BinaryExprAST* expr);

    /// @brief Evaluate a unary expression.
    ConstantValue evalUnary(const UnaryExprAST* expr);

    /// @brief Evaluate a call expression (const function call).
    ConstantValue evalCall(const CallExprAST* expr);

    /// @brief Evaluate a struct literal.
    ConstantValue evalStructLiteral(const StructLiteralExprAST* expr);

    /// @brief Evaluate an array literal.
    ConstantValue evalArrayLiteral(const ArrayLiteralExprAST* expr);

    /// @brief Evaluate a field access.
    ConstantValue evalFieldAccess(const FieldAccessExprAST* expr);

    /// @brief Evaluate a null coalesce expression (??).
    ConstantValue evalNullCoalesce(const NullCoalesceExprAST* expr);

    /// @brief Evaluate an if expression.
    ConstantValue evalIfExpr(const IfExprAST* expr);

private:
    // ─── Members ──────────────────────────────────────────────────────

    SemaContext& m_ctx;

    // Track which expressions have been evaluated
    std::unordered_set<const ExprAST*> m_evaluatedExprs;

    // Dependency graph: declaration → declarations it depends on
    std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;

    // Currently evaluating (for cycle detection)
    std::unordered_set<const DeclAST*> m_evaluating;

    // Recursion limit
    static constexpr size_t MAX_RECURSION = 1000;
    size_t m_recursionDepth = 0;

    // ─── Type Helpers ──────────────────────────────────────────────────

    /// @brief Check if a type can be evaluated at compile-time.
    bool isEvaluableType(const TypeAST* type);

    /// @brief Get the constant type from a value.
    TypeAST* getConstantType(const ConstantValue& val);

    /// @brief Compare two constant values for equality.
    bool compareEqual(const ConstantValue& a, const ConstantValue& b);

    /// @brief Compare two constant values for ordering.
    int compareOrder(const ConstantValue& a, const ConstantValue& b);

    // ─── Dependency Analysis ─────────────────────────────────────────

    /// @brief Build dependency graph for all const declarations.
    void buildDependencyGraph();

    /// @brief Collect dependencies of an expression.
    void collectDeps(const ExprAST* expr, std::vector<const DeclAST*>& deps);

    /// @brief Topological sort of const declarations.
    std::vector<const DeclAST*> topologicalSort();

    /// @brief Detect cycles in the dependency graph.
    bool detectCycle(std::vector<const DeclAST*>& cycle);

    // ─── Error Reporting ──────────────────────────────────────────────

    /// @brief Report a const evaluation error.
    void reportError(const BaseAST* node, const std::string& msg);

    /// @brief Report a dependency cycle.
    void reportCycle(const std::vector<const DeclAST*>& cycle);

    /// @brief Get the string representation of a constant value.
    std::string valueToString(const ConstantValue& val) const;
};

} // namespace sema