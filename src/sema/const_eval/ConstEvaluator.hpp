/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Integrated with SemaContext
///   Uses ContextStack for function/if/loop contexts, SymbolStorage for
///   local variables, and NarrowingStack for type narrowing. No custom
///   frame system - leverages the existing semantic analysis infrastructure.
///
/// @design_decision Called during type resolution (Phase 2)
///   Const evaluation happens when resolving declarations, not as a
///   separate phase. This allows the evaluator to use all the context
///   information already available.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../support/TypeNarrowHelpers.hpp"
#include "../context/SemaContext.hpp"
#include "../types/SemaType.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
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

/// @brief RAII guard for const evaluation function context.
/// 
/// Pushes a function context and scope for const function execution.
/// Uses the existing ContextStack and SymbolStorage.
class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, const FuncDeclAST* func)
        : m_ctx(ctx) {
        // Push function context using existing system
        m_ctx.contexts.pushFunction(
            const_cast<FuncDeclAST*>(func),
            const_cast<FuncTypeAST*>(func->funcType),
            func->loc
        );
        // Push scope for local variables
        m_ctx.symbols.pushScope();
    }
    
    ~ConstFunctionContext() {
        m_ctx.symbols.popScope();
        m_ctx.contexts.pop();
    }
    
    ConstFunctionContext(const ConstFunctionContext&) = delete;
    ConstFunctionContext& operator=(const ConstFunctionContext&) = delete;

private:
    SemaContext& m_ctx;
};

/// @brief RAII guard for const evaluation if context.
/// 
/// Uses the existing ScopedIfCondition for type narrowing.
class ConstIfContext {
public:
    ConstIfContext(SemaContext& ctx, bool hasElse)
        : m_guard(ctx, hasElse) {}
    
private:
    ScopedIfCondition m_guard;
};

/// @brief RAII guard for const evaluation narrowing.
/// 
/// Uses the existing ScopedNarrowing for type narrowing.
class ConstNarrowing {
public:
    ConstNarrowing(SemaContext& ctx, InternedString name, 
                   const TypeAST* narrowedType, bool isInverse = false)
        : m_guard(ctx, name, narrowedType, isInverse) {}
    
private:
    ScopedNarrowing m_guard;
};

/// @brief Evaluates const declarations at compile-time.
///
/// This class is tightly integrated with SemaContext and uses:
///   - ContextStack for function/if/loop/block contexts
///   - SymbolStorage for local variable binding
///   - NarrowingStack for type narrowing
///   - ReturnRequirements for return tracking
///   - The existing diagnostic system for errors
class ConstEvaluator {
public:
    explicit ConstEvaluator(SemaContext& ctx);

    /// @brief Main entry point: evaluate all const declarations.
    void evaluateAll();

    /// @brief Evaluate a specific const declaration.
    /// Called from resolveVarDecl during type resolution.
    ConstantValue evaluateDecl(const VarDeclAST* decl);

    /// @brief Check if an expression has been evaluated.
    bool isEvaluated(const ExprAST* expr) const;

    /// @brief Get the evaluated value of an expression.
    ConstantValue getValue(const ExprAST* expr) const;

    /// @brief Get the SemaContext.
    SemaContext& context() { return m_ctx; }

    // ─── Expression Evaluation ───────────────────────────────────────

    /// @brief Evaluate an expression and store result on the expression.
    /// 
    /// Uses the existing context for lookups (SymbolStorage, ContextStack).
    /// Type narrowing is automatically applied via NarrowingStack.
    ConstantValue evalExpr(const ExprAST* expr);

    /// @brief Evaluate a literal expression.
    ConstantValue evalLiteral(const LiteralExprAST* expr);

    /// @brief Evaluate an identifier expression.
    /// Uses SymbolStorage for lookup, respecting narrowing.
    ConstantValue evalIdentifier(const IdentifierExprAST* expr);

    /// @brief Evaluate a binary expression.
    /// Uses ContextStack to detect narrowing patterns.
    ConstantValue evalBinary(const BinaryExprAST* expr);

    /// @brief Evaluate a unary expression.
    ConstantValue evalUnary(const UnaryExprAST* expr);

    /// @brief Evaluate a call expression (const function call).
    /// Uses ContextStack for function context.
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
    /// Uses ScopedIfCondition and ScopedNarrowing for type narrowing.
    ConstantValue evalIfExpr(const IfExprAST* expr);

    // ─── Statement Execution ───────────────────────────────────────────

    /// @brief Execute a statement in the current context.
    /// Uses ContextStack for context tracking.
    ConstantValue executeStmt(const StmtAST* stmt);

    /// @brief Execute a const function with constant arguments.
    /// Uses ConstFunctionContext for context management.
    ConstantValue executeFunction(const FuncDeclAST* func,
                                   const std::vector<ConstantValue>& args);

    // ─── Integration with SemaDecl ────────────────────────────────────

    /// @brief Called from resolveVarDecl to evaluate const variables.
    /// This is the main integration point.
    void evaluateConstVar(const VarDeclAST* decl) {
        if (decl->keyword == DeclKeyword::Const && decl->init) {
            ConstantValue val = evalExpr(decl->init);
            if (!val.isError()) {
                // Store the value on the initializer expression (ExprAST is mutable)
                const_cast<ExprAST*>(decl->init)->isConst = true;
                const_cast<ExprAST*>(decl->init)->constValue = val;
                const_cast<VarDeclAST*>(decl)->isConst = true;
            }
        }
    }

private:
    // ─── Members ──────────────────────────────────────────────────────

    SemaContext& m_ctx;

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

    // ─── Binary Operation Helpers ────────────────────────────────────

    /// @brief Evaluate a binary operation on constant values.
    ConstantValue evalBinaryOp(BinaryOp op,
                                const ConstantValue& left,
                                const ConstantValue& right,
                                const BaseAST* node);

    // ─── Local Variable Helpers ──────────────────────────────────────

    /// @brief Get a local variable value from SymbolStorage.
    ConstantValue getLocalValue(InternedString name) const;

    /// @brief Set a local variable value in SymbolStorage.
    void setLocalValue(InternedString name, const ConstantValue& value);

    /// @brief Check if a name is a local variable in the current scope.
    bool isLocalVariable(InternedString name) const;

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