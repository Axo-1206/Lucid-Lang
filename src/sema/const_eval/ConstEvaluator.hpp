/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Single-pass evaluation after type resolution
///   All types are already resolved by Phase 2. The evaluator uses
///   the resolved types directly and stores results on AST nodes.
///
/// @design_decision Frame-based execution model
///   Uses a simple stack of frames for function calls. Each frame
///   contains local variable bindings and return state.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"
#include "../types/SemaType.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>
#include <functional>

namespace sema {

/// @brief RAII guard for const evaluation recursion tracking.
class EvaluationGuard {
public:
    EvaluationGuard(std::unordered_set<const DeclAST*>& evaluating,
                    const DeclAST* decl)
        : m_evaluating(evaluating), m_decl(decl) {
        m_evaluating.insert(decl);
    }
    
    ~EvaluationGuard() {
        m_evaluating.erase(m_decl);
    }
    
    EvaluationGuard(const EvaluationGuard&) = delete;
    EvaluationGuard& operator=(const EvaluationGuard&) = delete;
    EvaluationGuard(EvaluationGuard&&) = delete;
    EvaluationGuard& operator=(EvaluationGuard&&) = delete;

private:
    std::unordered_set<const DeclAST*>& m_evaluating;
    const DeclAST* m_decl;
};

/// @brief Evaluates const declarations at compile-time.
class ConstEvaluator {
public:
    explicit ConstEvaluator(SemaContext& ctx);

    /// @brief Main entry point: evaluate all const declarations.
    void evaluateAll();

    /// @brief Evaluate a specific const declaration.
    ConstantValue evaluateDecl(const DeclAST* decl);

    /// @brief Check if an expression has been evaluated.
    bool isEvaluated(const ExprAST* expr) const;

    /// @brief Get the evaluated value of an expression.
    ConstantValue getValue(const ExprAST* expr) const;

    /// @brief Get the SemaContext.
    SemaContext& context() { return m_ctx; }

    // ─── Expression Evaluation ───────────────────────────────────────

    /// @brief Evaluate an expression and store result on the expression.
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

    /// @brief Evaluate a null coalesce expression.
    ConstantValue evalNullCoalesce(const NullCoalesceExprAST* expr);

    /// @brief Evaluate an if expression.
    ConstantValue evalIfExpr(const IfExprAST* expr);

    // ─── Statement Execution ───────────────────────────────────────────

    /// @brief Execute a statement in the current context.
    ConstantValue executeStmt(const StmtAST* stmt);

    /// @brief Execute a const function with constant arguments.
    ConstantValue executeFunction(const FuncDeclAST* func,
                                   const std::vector<ConstantValue>& args);

private:
    // ─── Frame for function execution ─────────────────────────────────

    struct Frame {
        std::unordered_map<InternedString, ConstantValue> locals;
        bool hasReturned = false;
        ConstantValue returnValue;
    };

    // ─── Members ──────────────────────────────────────────────────────

    SemaContext& m_ctx;

    // Stack of frames for function execution
    std::vector<Frame> m_frames;

    // Track which expressions have been evaluated
    std::unordered_set<const ExprAST*> m_evaluatedExprs;

    // Dependency graph: declaration → declarations it depends on
    std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;

    // Currently evaluating (for cycle detection)
    std::unordered_set<const DeclAST*> m_evaluating;

    // All const declarations in order
    std::vector<const DeclAST*> m_constDecls;

    // Recursion limit
    static constexpr size_t MAX_RECURSION = 1000;
    size_t m_recursionDepth = 0;

    // ─── Frame Management ─────────────────────────────────────────────

    Frame& currentFrame() { return m_frames.back(); }
    const Frame& currentFrame() const { return m_frames.back(); }

    void pushFrame() { m_frames.emplace_back(); }
    void popFrame() { m_frames.pop_back(); }

    ConstantValue getLocal(InternedString name) const;
    void setLocal(InternedString name, const ConstantValue& value);

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

    /// @brief Collect dependencies from a statement.
    void collectDepsFromStmt(const StmtAST* stmt, std::vector<const DeclAST*>& deps);

    /// @brief Topological sort of const declarations using Kahn's algorithm.
    std::vector<const DeclAST*> topologicalSort();

    // ─── Statement Execution Helpers ──────────────────────────────────

    ConstantValue executeBlock(const BlockStmtAST* block);
    ConstantValue executeReturn(const ReturnStmtAST* stmt);
    ConstantValue executeIf(const IfStmtAST* stmt);
    ConstantValue executeWhile(const WhileStmtAST* stmt);
    ConstantValue executeAssign(const AssignExprAST* stmt);
    ConstantValue executeExprStmt(const ExprStmtAST* stmt);
    ConstantValue executeDeclStmt(const DeclStmtAST* stmt);
    ConstantValue executeBreak();
    ConstantValue executeContinue();

    // ─── Error Reporting ──────────────────────────────────────────────

    /// @brief Report a const evaluation error.
    ConstantValue error(const BaseAST* node, const std::string& msg);

    /// @brief Report a dependency cycle.
    void reportCycle(const std::vector<const DeclAST*>& cycle);

    /// @brief Get the string representation of a constant value.
    std::string valueToString(const ConstantValue& val) const;
};

} // namespace sema