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

class ConstFunctionContext {
public:
    ConstFunctionContext(SemaContext& ctx, const FuncDeclAST* func)
        : m_ctx(ctx) {
        m_ctx.stack.pushFunction(
            const_cast<FuncDeclAST*>(func),
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
class ConstEvaluator {
public:
    static constexpr size_t MAX_RECURSION = 1000;
    static constexpr size_t MAX_ITERATIONS = 10000;

    // ─── Main Entry Points ───────────────────────────────────────────────

    /// @brief Evaluate a const variable declaration.
    static ConstantValue evaluateDecl(SemaContext& ctx, const VarDeclAST* decl);

    /// @brief Evaluate an expression with optional target type.
    /// 
    /// This is the main entry point for evaluating any expression.
    /// It uses caching to avoid re-evaluating the same expression.
    /// 
    /// @param ctx The semantic context.
    /// @param expr The expression to evaluate.
    /// @param targetType Optional expected type (for type checking).
    /// @return The evaluated constant value, or error/unknown on failure.
    static ConstantValue evaluate(SemaContext& ctx, const ExprAST* expr,
                                  const TypeAST* targetType = nullptr);

    /// @brief Check if an expression is compile-time constant.
    static bool isConstExpr(SemaContext& ctx, const ExprAST* expr,
                            const TypeAST* targetType = nullptr);

    /// @brief Get the constant value of an expression if it's const.
    static ConstantValue getConstValue(SemaContext& ctx, const ExprAST* expr,
                                       const TypeAST* targetType = nullptr);

    /// @brief Evaluate an expression as an integer.
    static std::optional<int64_t> evaluateAsInt(SemaContext& ctx, const ExprAST* expr);

    /// @brief Evaluate an expression as a boolean.
    static std::optional<bool> evaluateAsBool(SemaContext& ctx, const ExprAST* expr);

    /// @brief Report a circular dependency.
    static void reportCycle(SemaContext& ctx, const std::vector<const DeclAST*>& cycle);

    /// @brief Build the dependency graph for const declarations.
    static void buildDependencyGraph(SemaContext& ctx);

    /// @brief Get the const value of a declaration.
    static ConstantValue getConstValue(const VarDeclAST* decl);

    // ─── Binary Operation Evaluators ────────────────────────────────────

    static ConstantValue evalAdd(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalSub(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalMul(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalDiv(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalMod(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalPow(SemaContext& ctx, const ConstantValue& left,
                                  const ConstantValue& right,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalNeg(SemaContext& ctx, const ConstantValue& operand,
                                  const BaseAST* node,
                                  const TypeAST* targetType);

    static ConstantValue evalNot(SemaContext& ctx, const ConstantValue& operand,
                                  const BaseAST* node);

    static ConstantValue evalBitNot(SemaContext& ctx, const ConstantValue& operand,
                                     const BaseAST* node);

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
    static ConstantValue evalRangeExpr(SemaContext& ctx, const RangeExprAST* expr);

    // ─── Statement Execution (for const functions) ──────────────────────

    static ConstantValue executeStmt(SemaContext& ctx, const StmtAST* stmt);
    static ConstantValue executeBlock(SemaContext& ctx, const BlockStmtAST* block);
    static ConstantValue executeReturn(SemaContext& ctx, const ReturnStmtAST* stmt);
    static ConstantValue executeIf(SemaContext& ctx, const IfStmtAST* stmt);
    static ConstantValue executeWhile(SemaContext& ctx, const WhileStmtAST* stmt);
    static ConstantValue executeFor(SemaContext& ctx, const ForStmtAST* stmt);
    static ConstantValue executeSwitch(SemaContext& ctx, const SwitchStmtAST* stmt);
    static ConstantValue executeExprStmt(SemaContext& ctx, const ExprStmtAST* stmt);
    static ConstantValue executeDeclStmt(SemaContext& ctx, const DeclStmtAST* stmt);

    static ConstantValue executeFunction(SemaContext& ctx, const FuncDeclAST* func,
                                          const std::vector<ConstantValue>& args);

    // ─── Binary Operation Dispatcher ────────────────────────────────────

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

    // ─── Internal State ──────────────────────────────────────────────────

    static std::vector<const DeclAST*> m_constDecls;
    static std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> m_deps;
    static std::unordered_set<const ExprAST*> m_evaluatedExprs;
    static std::unordered_set<const DeclAST*> m_evaluating;
    static size_t m_recursionDepth;
};

} // namespace sema