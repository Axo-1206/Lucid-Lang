/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Static methods only - no instance state needed.
///   Each const declaration is evaluated independently. The evaluator
///   doesn't need to persist state between calls.
///
/// @design_decision Silent on "not const-evaluable"
///   If an expression can't be evaluated at compile time, we simply return
///   ConstantValue::unknown() without emitting a diagnostic. This is normal
///   and expected - the compiler continues with regular type checking.
///   Only actual errors (division by zero, circular dependencies, etc.)
///   trigger diagnostics.
///
/// @design_decision Uses existing semantic infrastructure
///   - Type checking: SemaCompare (typesEqual, isAssignable, isNullableType, etc.)
///   - Name lookup: SemaContext (lookupValue, lookupType, etc.)
///   - Type resolution: SemaResolve (resolveType, etc.)
///   - Context management: SemaContext (pushFunction, pushScope, etc.)
///   This eliminates duplication and ensures consistency between compile-time
///   and runtime behavior.
///
/// @design_decision No separate ConstFrame
///   Local variables are stored on the AST nodes themselves (constValue field
///   on ExprAST) and looked up via SemaContext::lookupValue. This eliminates
///   duplication with SemaContext's Scope system.
///
/// # Error Handling Strategy
///
/// ## Hard Errors (Diagnostic + Error Return)
/// 
/// These are unrecoverable errors that make the const expression invalid:
///   - Division by zero
///   - Integer overflow
///   - Circular dependency
///   - Missing initializer
///   - Type mismatch
///   - Invalid operation
///   - Shift amount >= bit width
///   - Negative shift amount
///   - Recursion limit exceeded
///   - Unknown identifier (should be caught by name resolution)
///
/// ## Soft Errors (No Diagnostic + Unknown Return)
/// 
/// These are cases where the expression can't be evaluated at compile time,
/// but this is normal and expected:
///   - Non-const variable reference
///   - Non-const function call
///   - Unknown expression kind
///   - Value that depends on runtime input
///
/// ## Why This Strategy?
/// 
/// 1. **Hard Errors** - The expression is invalid and cannot be used.
///    Example: `const x = 1 / 0` → This is never valid.
/// 
/// 2. **Soft Errors** - The expression is valid but not const-evaluable.
///    Example: `const x = y + 1` where `y` is a runtime variable.
///    The compiler should silently fall back to runtime evaluation.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"
#include "../support/TypeNarrowHelpers.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// RAII Guards
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief RAII guard for const evaluation recursion/cycle detection.
 * 
 * WHY: Prevents infinite loops from circular const dependencies.
 * 
 * @example
 *   const x = y + 1
 *   const y = x + 1  // ← EvaluationGuard catches this cycle
 * 
 * HOW: Inserts the declaration into a set on construction, removes on destruction.
 *      If a declaration is already in the set, it's a circular dependency.
 */
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

private:
    std::unordered_set<const DeclAST*>& m_evaluating;
    const DeclAST* m_decl;
};

/**
 * @brief RAII guard for const function execution context.
 * 
 * WHY: Const functions need proper semantic context to execute.
 *       - pushFunction: Enables `return` statement validation
 *       - pushScope: Creates scope for function parameters
 */
class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, const FuncDeclAST* func)
        : m_ctx(ctx) {
        // Push function context for return validation
        m_ctx.stack.pushFunction(
            const_cast<FuncDeclAST*>(func),
            func->funcType ? func->funcType->returnType : nullptr,
            func->loc
        );
        // Push scope for parameters
        m_ctx.pushScope();
    }
    
    ~ConstFunctionContext() {
        m_ctx.popScope();
        m_ctx.stack.pop();
    }

private:
    SemaContext& m_ctx;
};

// ─────────────────────────────────────────────────────────────────────────────
// ConstEvaluator - Main Class
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Evaluates const declarations at compile-time.
/// All methods are static - no instance state needed.
class ConstEvaluator {
public:
    static constexpr size_t MAX_RECURSION = 1000;

    // ─── Main Entry Points ───────────────────────────────────────────────

    /// @brief Evaluate a const variable declaration.
    /// 
    /// @param ctx The semantic context.
    /// @param decl The const variable declaration.
    /// @return The evaluated constant value, or error/unknown on failure.
    /// 
    /// @note Hard errors emit diagnostics. Soft errors return unknown.
    static ConstantValue evaluateDecl(SemaContext& ctx, const VarDeclAST* decl);

    /// @brief Evaluate an expression with optional target type.
    /// 
    /// @param ctx The semantic context.
    /// @param expr The expression to evaluate.
    /// @param targetType Optional expected type (for type checking).
    /// @return The evaluated constant value, or error/unknown on failure.
    /// 
    /// @note Hard errors emit diagnostics. Soft errors return unknown.
    static ConstantValue evaluate(SemaContext& ctx, const ExprAST* expr,
                                  const TypeAST* targetType = nullptr);

    /// @brief Report a circular dependency in const declarations.
    static void reportCycle(SemaContext& ctx, const std::vector<const DeclAST*>& cycle);

    /// @brief Build the dependency graph for const declarations.
    static void buildDependencyGraph(SemaContext& ctx);

    /// @brief Get the const value of a declaration if it has one.
    static ConstantValue getConstValue(const VarDeclAST* decl);

private:
    // ─── Expression Evaluators ──────────────────────────────────────────

    static ConstantValue evalLiteral(SemaContext& ctx, const LiteralExprAST* expr);
    static ConstantValue evalIdentifier(SemaContext& ctx, const IdentifierExprAST* expr);
    static ConstantValue evalBinary(SemaContext& ctx, const BinaryExprAST* expr,
                                     const TypeAST* targetType);
    static ConstantValue evalUnary(SemaContext& ctx, const UnaryExprAST* expr,
                                    const TypeAST* targetType);
    static ConstantValue evalCall(SemaContext& ctx, const CallExprAST* expr);
    static ConstantValue evalStructLiteral(SemaContext& ctx, const StructLiteralExprAST* expr);
    static ConstantValue evalArrayLiteral(SemaContext& ctx, const ArrayLiteralExprAST* expr);
    static ConstantValue evalFieldAccess(SemaContext& ctx, const FieldAccessExprAST* expr);
    static ConstantValue evalNullCoalesce(SemaContext& ctx, const NullCoalesceExprAST* expr);
    static ConstantValue evalIfExpr(SemaContext& ctx, const IfExprAST* expr);

    // ─── Statement Execution ─────────────────────────────────────────────

    static ConstantValue executeStmt(SemaContext& ctx, const StmtAST* stmt);
    static ConstantValue executeBlock(SemaContext& ctx, const BlockStmtAST* block);
    static ConstantValue executeReturn(SemaContext& ctx, const ReturnStmtAST* stmt);
    static ConstantValue executeIf(SemaContext& ctx, const IfStmtAST* stmt);
    static ConstantValue executeWhile(SemaContext& ctx, const WhileStmtAST* stmt);
    static ConstantValue executeExprStmt(SemaContext& ctx, const ExprStmtAST* stmt);
    static ConstantValue executeDeclStmt(SemaContext& ctx, const DeclStmtAST* stmt);

    /// @brief Execute a const function with constant arguments.
    static ConstantValue executeFunction(SemaContext& ctx, const FuncDeclAST* func,
                                          const std::vector<ConstantValue>& args);

    // ─── Binary Operation Helpers ───────────────────────────────────────

    static ConstantValue evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                       const ConstantValue& left,
                                       const ConstantValue& right,
                                       const BaseAST* node,
                                       const TypeAST* targetType);

    // ─── Comparison Helpers ──────────────────────────────────────────────

    static bool compareEqual(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b);
    static int compareOrder(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b);

    // ─── Type Helpers ────────────────────────────────────────────────────

    static TypeAST* getConstantType(SemaContext& ctx, const ConstantValue& val);

    // ─── Dependency Analysis ─────────────────────────────────────────────

    static void collectDeps(SemaContext& ctx, const ExprAST* expr, 
                            std::vector<const DeclAST*>& deps);
    static void collectDepsFromStmt(SemaContext& ctx, const StmtAST* stmt,
                                     std::vector<const DeclAST*>& deps);
    static std::vector<const DeclAST*> topologicalSort(SemaContext& ctx);

    // ─── Internal State ──────────────────────────────────────────────────

    // Dependency graph for const declarations (for cycle detection)
    static std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;
    
    // Set of expressions that have already been evaluated (caching)
    static std::unordered_set<const ExprAST*> m_evaluatedExprs;
    
    // Set of declarations currently being evaluated (cycle detection)
    static std::unordered_set<const DeclAST*> m_evaluating;
    
    // Current recursion depth (prevents stack overflow)
    static size_t m_recursionDepth;
};

} // namespace sema