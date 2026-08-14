/// @file const_eval/ConstEvaluator.hpp
/// @brief Evaluates const expressions at compile-time.
///
/// @design_decision Single responsibility: evaluate expression → ConstantValue
///   The evaluator does not know about statements, loops, or switches.
///   It only evaluates expressions and returns their constant values.
///
/// @design_decision Results are stored on the AST node
///   When an expression is evaluated, we set expr->isConst = true and
///   expr->constValue = result. This avoids re-evaluation.
///
/// @design_decision Unknown is not an error
///   If an expression can't be evaluated, we return ConstantValue::unknown()
///   without a diagnostic. The caller decides what to do.
///
/// @design_decision Mutable AST nodes
///   AST nodes use mutable fields now (removed const from parser fields).
///   The const evaluator modifies AST nodes directly by setting isConst,
///   constValue, and resolvedType on expressions it evaluates.

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
class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, FuncDeclAST* func)
        : m_ctx(ctx) {
        m_ctx.stack.pushFunction(
            func,
            func->funcType ? func->funcType->returnType : nullptr
        );
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

/// @brief Evaluates const expressions at compile-time.
/// All methods are static - no instance state needed.
/// 
/// ─── Phase Responsibilities ──────────────────────────────────────────────
/// | Field            | Set By      | Read By               | Notes                     |
/// | -----------------| ----------- | --------------------- | ------------------------- |
/// | `isConst`        | Evaluator   | CodeGen               | True if const evaluated   |
/// | `constValue`     | Evaluator   | CodeGen               | The evaluated constant    |
/// | `resolvedType`   | Evaluator   | Sema, CodeGen         | Set during evaluation     |
/// | `valueState`     | Evaluator   | Sema, CodeGen         | Nil/Err/Definite/Unknown  |
class ConstEvaluator {
public:
    static constexpr size_t MAX_RECURSION = 1000;
    static constexpr size_t MAX_ITERATIONS = 10000;

    // ─── Main Entry Points ───────────────────────────────────────────────

    /// @brief Evaluate a const variable declaration.
    /// Sets decl->constValue and decl->isConst.
    static ConstantValue evaluateDecl(SemaContext& ctx, VarDeclAST* decl);

    /// @brief Evaluate an expression with optional target type.
    /// 
    /// This is the main entry point for evaluating any expression.
    /// It uses caching to avoid re-evaluating the same expression.
    /// Sets expr->isConst, expr->constValue, expr->resolvedType.
    /// 
    /// @param ctx The semantic context.
    /// @param expr The expression to evaluate.
    /// @param targetType Optional expected type (for type checking).
    /// @return The evaluated constant value, or error/unknown on failure.
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

    /// @brief Get the const value of a declaration.
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
    static ConstantValue evalIntrinsicCall(SemaContext& ctx, IntrinsicCallExprAST* expr);

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

    // ─── Type Helpers ────────────────────────────────────────────────────

    static TypeAST* getConstantType(SemaContext& ctx, const ConstantValue& val);

    // ─── Internal State ──────────────────────────────────────────────────

    static std::vector<DeclAST*> m_constDecls;
    static std::unordered_map<DeclAST*, std::vector<DeclAST*>> m_deps;
    static std::unordered_set<ExprAST*> m_evaluatedExprs;
    static std::unordered_set<DeclAST*> m_evaluating;
    static size_t m_recursionDepth;
};

} // namespace sema