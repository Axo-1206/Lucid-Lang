/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Single responsibility: evaluate expression → ConstantValue
///   The evaluator does not know about statements, loops, or switches.
///   It only evaluates expressions and returns their constant values.
///
/// @design_decision Results are cached internally
///   When an expression is evaluated, we store the result in m_evalCache.
///   This avoids re-evaluation without bloating the AST with heavy data.
///
/// @design_decision Unknown is not an error
///   If an expression can't be evaluated, we return ConstantValue::unknown()
///   without a diagnostic. The caller decides what to do.
///
/// @design_decision AST nodes are minimally modified
///   We only set `isConst = true` on successfully evaluated expressions.
///   The actual value is stored in the evaluator's internal cache.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"
#include "../support/TypeNarrowHelpers.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// RAII Guards
// ─────────────────────────────────────────────────────────────────────────────

/// @brief RAII guard for tracking declarations being evaluated.
/// Prevents infinite recursion in circular dependencies.
class EvaluationGuard {
public:
    EvaluationGuard(std::unordered_set<DeclAST*>& evaluating,
                    DeclAST* decl)
        : m_evaluating(evaluating), m_decl(decl) {
        m_evaluating.insert(decl);
    }
    
    ~EvaluationGuard() {
        m_evaluating.erase(m_decl);
    }
    
    EvaluationGuard(const EvaluationGuard&) = delete;
    EvaluationGuard& operator=(const EvaluationGuard&) = delete;

private:
    std::unordered_set<DeclAST*>& m_evaluating;
    DeclAST* m_decl;
};

/// @brief RAII guard for const function evaluation context.
/// Pushes a function context and scope for evaluating const functions.
///
/// This guard therefore requires `func->init` to be an AnonFuncExprAST. If
/// `func`'s init is a reference (a pure alias to another function) or null
/// (a foreign declaration), there is no body to execute, and the guard is
/// a no-op — which is exactly right: there is nothing to evaluate.
class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, FuncDeclAST* func)
        : m_ctx(ctx)
        , m_pushed(false)
    {
        if (!func || !func->init) {
            return;   // nothing to push — foreign or missing body
        }
        if (!func->init->isa<AnonFuncExprAST>()) {
            return;   // reference body — no body of its own to evaluate
        }

        AnonFuncExprAST* body = func->init->as<AnonFuncExprAST>();

        m_ctx.stack.pushAnonFunction(
            body,
            body->funcType ? body->funcType->returnType : nullptr
        );
        m_ctx.pushScope();
        m_pushed = true;
    }

    ~ConstFunctionContext() {
        if (m_pushed) {
            m_ctx.popScope();
            m_ctx.stack.pop();
        }
    }

private:
    SemaContext& m_ctx;
    bool m_pushed;
};

/// @brief RAII guard for recursion depth tracking.
class DepthGuard {
public:
    DepthGuard(size_t& depth) : m_depth(depth) { ++m_depth; }
    ~DepthGuard() { --m_depth; }
    
private:
    size_t& m_depth;
};

// ─────────────────────────────────────────────────────────────────────────────
// ConstEvaluator - Main Class
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Evaluates const expressions at compile-time.
/// All methods are static - no instance state needed.
/// @note Callers should provide an AST free of syntax errors. Public entry
///       points defensively reject nodes marked with `hasSyntaxError`.
/// 
/// ─── Phase Responsibilities ──────────────────────────────────────────────
/// | Field            | Set By      | Read By               | Notes                     |
/// | -----------------| ----------- | --------------------- | ------------------------- |
/// | `isConst`        | Evaluator   | Sema, CodeGen         | True if const evaluated   |
/// | `resolvedType`   | Evaluator   | Sema, CodeGen         | Set during evaluation     |
/// | `valueState`     | Evaluator   | Sema, CodeGen         | Nil/Err/Definite/Unknown  |
class ConstEvaluator {
public:
    static constexpr size_t MAX_RECURSION = 1000;
    static constexpr size_t MAX_ITERATIONS = 10000;

    // ─── Main Entry Points ───────────────────────────────────────────────

    /// @brief Evaluate a const variable declaration.
    /// @note Syntax-broken declarations are rejected without new diagnostics.
    static ConstantValue evaluateDecl(SemaContext& ctx, VarDeclAST* decl);

    /// @brief Evaluate an expression with optional target type.
    /// 
    /// This is the main entry point for evaluating any expression.
    /// It uses an internal cache to avoid re-evaluating the same expression.
    /// Sets expr->isConst = true and expr->resolvedType on success.
    /// 
    /// @param ctx The semantic context.
    /// @param expr The expression to evaluate.
    /// @param targetType Optional expected type (for type checking).
    /// @return The evaluated constant value, or error/unknown on failure.
    /// @note Syntax-broken expressions are rejected without new diagnostics.
    static ConstantValue evaluate(SemaContext& ctx, ExprAST* expr,
                                  TypeAST* targetType = nullptr);

    /// @brief Check if an expression is compile-time constant.
    static bool isConstExpr(SemaContext& ctx, ExprAST* expr,
                            TypeAST* targetType = nullptr);

    /// @brief Get the constant value of an expression if it's const.
    static ConstantValue getConstValue(SemaContext& ctx, ExprAST* expr,
                                       TypeAST* targetType = nullptr);

    /// @brief Evaluate an expression as an integer.
    static std::optional<int64_t> evaluateAsInt(SemaContext& ctx, ExprAST* expr);

    /// @brief Evaluate an expression as a boolean.
    static std::optional<bool> evaluateAsBool(SemaContext& ctx, ExprAST* expr);

    /// @brief Report a circular dependency.
    static void reportCycle(SemaContext& ctx, const std::vector<DeclAST*>& cycle);

    /// @brief Build the dependency graph for const declarations.
    static void buildDependencyGraph(SemaContext& ctx);

    /// @brief Get the const value of a declaration from the cache.
    static ConstantValue getConstValue(VarDeclAST* decl);

    // ─── Binary Operation Evaluators ────────────────────────────────────

    static ConstantValue evalAdd(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalSub(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalMul(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalDiv(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalMod(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalPow(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalNeg(SemaContext& ctx, const ConstantValue& operand,
                                  BaseAST* node,
                                  TypeAST* targetType);

    static ConstantValue evalNot(SemaContext& ctx, const ConstantValue& operand,
                                  BaseAST* node);

    static ConstantValue evalBitNot(SemaContext& ctx, const ConstantValue& operand,
                                     BaseAST* node);

private:
    // ─── Expression Evaluators ──────────────────────────────────────────

    static ConstantValue evalLiteral(SemaContext& ctx, LiteralExprAST* expr);
    static ConstantValue evalIdentifier(SemaContext& ctx, IdentifierExprAST* expr);
    static ConstantValue evalBinary(SemaContext& ctx, BinaryExprAST* expr,
                                     TypeAST* targetType);
    static ConstantValue evalUnary(SemaContext& ctx, UnaryExprAST* expr,
                                    TypeAST* targetType);
    static ConstantValue evalCall(SemaContext& ctx, CallExprAST* expr);
    static ConstantValue evalStructLiteral(SemaContext& ctx, StructLiteralExprAST* expr);
    static ConstantValue evalArrayLiteral(SemaContext& ctx, ArrayLiteralExprAST* expr);
    static ConstantValue evalFieldAccess(SemaContext& ctx, FieldAccessExprAST* expr);
    static ConstantValue evalNullCoalesce(SemaContext& ctx, NullCoalesceExprAST* expr);
    static ConstantValue evalIfExpr(SemaContext& ctx, IfExprAST* expr);
    static ConstantValue evalRangeExpr(SemaContext& ctx, RangeExprAST* expr);

    // ─── Intrinsic Folding ──────────────────────────────────────────────
    //
    // Compiler-handled intrinsics whose value is fully determined at
    // compile time are evaluated here. Intrinsics that need codegen
    // (#sqrt, #memcpy, ...) return ConstantValue::unknown() so the
    // caller can fall back to the intrinsic's normal return type.
    //
    // Four intrinsics are foldable today:
    //   #typeof(T)   -> string literal naming the resolved type
    //   #nameof(x)   -> string literal naming the entity
    //   #sizeof(T)   -> int64, only for primitive types
    //   #alignof(T)  -> int64, only for primitive types
    //
    // Non-foldable intrinsics (#sizeof(MyStruct), #tostr(x), ...) return
    // Unknown, which is a "cannot be folded" signal, not an error.
    static ConstantValue evalIntrinsicCall(SemaContext& ctx, IntrinsicCallExprAST* expr);
    static ConstantValue evalIntrinsicTypeof(SemaContext& ctx, IntrinsicCallExprAST* expr);
    static ConstantValue evalIntrinsicNameof(SemaContext& ctx, IntrinsicCallExprAST* expr);
    static ConstantValue evalIntrinsicSizeof(SemaContext& ctx, IntrinsicCallExprAST* expr);
    static ConstantValue evalIntrinsicAlignof(SemaContext& ctx, IntrinsicCallExprAST* expr);

    // ─── Statement Execution (for const functions) ──────────────────────

    static ConstantValue executeStmt(SemaContext& ctx, StmtAST* stmt);
    static ConstantValue executeBlock(SemaContext& ctx, BlockStmtAST* block);
    static ConstantValue executeReturn(SemaContext& ctx, ReturnStmtAST* stmt);
    static ConstantValue executeIf(SemaContext& ctx, IfStmtAST* stmt);
    static ConstantValue executeWhile(SemaContext& ctx, WhileStmtAST* stmt);
    static ConstantValue executeFor(SemaContext& ctx, ForStmtAST* stmt);
    static ConstantValue executeSwitch(SemaContext& ctx, SwitchStmtAST* stmt);
    static ConstantValue executeExprStmt(SemaContext& ctx, ExprStmtAST* stmt);
    static ConstantValue executeDeclStmt(SemaContext& ctx, DeclStmtAST* stmt);

    static ConstantValue executeFunction(SemaContext& ctx, FuncDeclAST* func,
                                          const std::vector<ConstantValue>& args);

    // ─── Binary Operation Dispatcher ────────────────────────────────────

    static ConstantValue evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                       const ConstantValue& left,
                                       const ConstantValue& right,
                                       BaseAST* node,
                                       TypeAST* targetType);

    // ─── Comparison Helpers ──────────────────────────────────────────────

    static bool compareEqual(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b);
    static int compareOrder(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b);

    // ─── Internal State ──────────────────────────────────────────────────
    // These are static because the evaluator is stateless across calls.
    // The cache stores evaluated expressions to avoid re-computation.

    static std::vector<DeclAST*> m_constDecls;
    static std::unordered_map<DeclAST*, std::vector<DeclAST*>> m_deps;
    static std::unordered_map<ExprAST*, ConstantValue> m_evalCache;  // Value cache
    static std::unordered_set<DeclAST*> m_evaluating;                 // Cycle detection
    static size_t m_recursionDepth;

    /// @brief Per-call parameter bindings during const function evaluation.
    ///
    /// Keyed by the ParamAST* the body's identifiers resolve to. Populated
    /// by executeFunction before body execution and erased after; the
    /// evaluator never leaves a stale entry behind, so a recursive call
    /// to the same function sees only its own bindings in this map.
    ///
    /// Why a side table rather than a field on ParamAST:
    ///   1. ParamAST is a parser-owned node. Adding semantic-only state to
    ///      it is the same leak the FuncDeclAST redesign removed (see the
    ///      "Two funcType Fields" note in DeclAST.hpp).
    ///   2. The value is a property of *this call*, not of the parameter.
    ///      Two recursive evaluations of the same function bind different
    ///      values to the same ParamAST*; a field on the node could only
    ///      hold one at a time.
    ///   3. The table mirrors m_evalCache's shape — evaluator-owned,
    ///      keyed by AST node, populated and torn down per evaluation —
    ///      so there is one pattern for "where does the const evaluator
    ///      stash per-node values" rather than two.
    static std::unordered_map<ParamAST*, ConstantValue> m_paramBindings;
};

} // namespace sema