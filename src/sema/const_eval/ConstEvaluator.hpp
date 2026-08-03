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
///   - Name lookup: SemaLookup (lookupValue, lookupType, etc.)
///   - Type resolution: SemaResolve (resolveType, etc.)
///   - Context management: SemaContext (pushFunction, pushScope, etc.)
///   This eliminates duplication and ensures consistency between compile-time
///   and runtime behavior.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"
#include "../support/TypeNarrowHelpers.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// RAII Guards - Why They Exist
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
 * 
 * @note This is the only guard that truly needs to exist as a separate class
 *       because it manages the static evaluation set.
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
 * 
 * @example
 *   const add (a int)(b int) -> int = { return a + b }
 *   //                    ↑
 *   // ConstFunctionContext sets up the context for executing this body
 * 
 * @note Could be simplified by inlining the push/pop calls, but kept as
 *       a separate class for clarity and to ensure proper cleanup.
 */
class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, const FuncDeclAST* func)
        : m_ctx(ctx) {
        // Push function context for return validation
        m_ctx.stack.pushFunction(
            const_cast<FuncDeclAST*>(func),
            const_cast<FuncTypeAST*>(func->funcType),
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

/**
 * @brief Frame for local variable values during const function execution.
 * 
 * WHY: Const functions can have local variables (e.g., `let y int = x + 1`).
 *      These need to be stored somewhere during execution.
 * 
 * @example
 *   const compute (x int) -> int = {
 *       let y int = x + 1  // ← Stored in ConstFrame
 *       return y * 2
 *   }
 * 
 * @note This could be simplified to just a `std::unordered_map` but is kept
 *       as a struct for clarity and to support future extensions (like
 *       tracking returns).
 */
struct ConstFrame {
    std::unordered_map<InternedString, ConstantValue> locals;
    bool hasReturned = false;
    ConstantValue returnValue;
};

// ─────────────────────────────────────────────────────────────────────────────
// REMOVED: ConstIfContext - Use ScopedIfCondition directly
// REMOVED: ConstNarrowing - Use ScopedNarrowing directly
// ─────────────────────────────────────────────────────────────────────────────

// These were removed because they were just thin wrappers around existing
// RAII guards in SemaContext:
//   - ConstIfContext  → ScopedIfCondition
//   - ConstNarrowing  → ScopedNarrowing

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
    static ConstantValue evaluateDecl(SemaContext& ctx, const VarDeclAST* decl);

    /// @brief Evaluate an expression with optional target type.
    static ConstantValue evaluate(SemaContext& ctx, const ExprAST* expr,
                                  const TypeAST* targetType = nullptr);

    /// @brief Report a circular dependency in const declarations.
    static void reportCycle(SemaContext& ctx, const std::vector<const DeclAST*>& cycle);

    /// @brief Build the dependency graph for const declarations.
    static void buildDependencyGraph(SemaContext& ctx);

private:
    // ─── Frame Management ────────────────────────────────────────────────

    static ConstFrame& currentFrame(std::vector<ConstFrame>& frames);
    static const ConstFrame& currentFrame(const std::vector<ConstFrame>& frames);
    static void pushFrame(std::vector<ConstFrame>& frames);
    static void popFrame(std::vector<ConstFrame>& frames);
    
    static ConstantValue getLocal(std::vector<ConstFrame>& frames, InternedString name);
    static void setLocal(std::vector<ConstFrame>& frames, InternedString name, const ConstantValue& value);
    static bool isLocalVariable(const std::vector<ConstFrame>& frames, InternedString name);

    // ─── Expression Evaluators ──────────────────────────────────────────

    static ConstantValue evalLiteral(SemaContext& ctx, const LiteralExprAST* expr);
    static ConstantValue evalIdentifier(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                         const IdentifierExprAST* expr);
    static ConstantValue evalBinary(SemaContext& ctx, const BinaryExprAST* expr,
                                     const TypeAST* targetType);
    static ConstantValue evalUnary(SemaContext& ctx, const UnaryExprAST* expr,
                                    const TypeAST* targetType);
    static ConstantValue evalCall(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                   const CallExprAST* expr);
    static ConstantValue evalStructLiteral(SemaContext& ctx, const StructLiteralExprAST* expr);
    static ConstantValue evalArrayLiteral(SemaContext& ctx, const ArrayLiteralExprAST* expr);
    static ConstantValue evalFieldAccess(SemaContext& ctx, const FieldAccessExprAST* expr);
    static ConstantValue evalNullCoalesce(SemaContext& ctx, const NullCoalesceExprAST* expr);
    static ConstantValue evalIfExpr(SemaContext& ctx, const IfExprAST* expr);

    // ─── Statement Execution ─────────────────────────────────────────────

    static ConstantValue executeStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                      const StmtAST* stmt);
    static ConstantValue executeBlock(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                       const BlockStmtAST* block);
    static ConstantValue executeReturn(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                        const ReturnStmtAST* stmt);
    static ConstantValue executeIf(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                    const IfStmtAST* stmt);
    static ConstantValue executeWhile(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                       const WhileStmtAST* stmt);
    static ConstantValue executeAssign(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                        const AssignExprAST* stmt);
    static ConstantValue executeExprStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                          const ExprStmtAST* stmt);
    static ConstantValue executeDeclStmt(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                          const DeclStmtAST* stmt);

    /// @brief Execute a const function with constant arguments.
    static ConstantValue executeFunction(SemaContext& ctx, std::vector<ConstFrame>& frames,
                                          const FuncDeclAST* func,
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

    // Static state shared across all evaluations
    static std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;
    static std::vector<const DeclAST*> m_constDecls;
    static std::unordered_set<const ExprAST*> m_evaluatedExprs;
    static std::unordered_set<const DeclAST*> m_evaluating;
    static size_t m_recursionDepth;
};

} // namespace sema