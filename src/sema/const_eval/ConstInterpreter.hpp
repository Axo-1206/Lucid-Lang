/// @file const_eval/ConstInterpreter.hpp
/// @brief Interprets const function bodies at compile-time.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "../context/SemaContext.hpp"
#include "../types/SemaType.hpp"

#include <unordered_map>
#include <vector>

namespace sema {

// Forward declarations
class ConstEvaluator;

/// @brief Interprets const function bodies at compile-time.
///
/// This is a mini-interpreter for const functions. It executes
/// function bodies with constant arguments and produces a constant result.
///
/// @design_decision Pure execution, no side effects
///   Const functions cannot have side effects (no I/O, no mutation).
///   The interpreter enforces this by rejecting non-pure operations.
///
/// @design_decision Simple stack frame model
///   Each function call creates a new frame with parameter bindings.
///   Local variables are stored in the frame.
class ConstInterpreter {
public:
    ConstInterpreter(ConstEvaluator& evaluator, SemaContext& ctx);

    /// @brief Execute a const function with constant arguments.
    ///
    /// @param func The const function to execute.
    /// @param args The constant arguments (must match parameter count).
    /// @return The result of the function execution.
    ConstantValue executeFunction(const FuncDeclAST* func,
                                   const std::vector<ConstantValue>& args);

    /// @brief Execute a const function body.
    ///
    /// @param body The function body (BlockStmtAST).
    /// @param params The function parameters.
    /// @param args The argument values (must match parameter count).
    /// @return The result of the body execution.
    ConstantValue executeBody(const StmtAST* body,
                               const std::vector<ParamAST*>& params,
                               const std::vector<ConstantValue>& args);

private:
    // ─── Members ──────────────────────────────────────────────────────

    ConstEvaluator& m_evaluator;
    SemaContext& m_ctx;

    // Local variable bindings for the current execution
    std::unordered_map<InternedString, ConstantValue> m_locals;

    // Return value from the current function
    ConstantValue m_returnValue;
    bool m_hasReturned = false;

    // ─── Statement Execution ──────────────────────────────────────────

    /// @brief Execute a statement.
    /// @return The result of the statement (if any), or void.
    ConstantValue executeStmt(const StmtAST* stmt);

    /// @brief Execute a block.
    ConstantValue executeBlock(const BlockStmtAST* block);

    /// @brief Execute a return statement.
    ConstantValue executeReturn(const ReturnStmtAST* stmt);

    /// @brief Execute an if statement.
    ConstantValue executeIf(const IfStmtAST* stmt);

    /// @brief Execute a while loop.
    ConstantValue executeWhile(const WhileStmtAST* stmt);

    /// @brief Execute an assignment.
    ConstantValue executeAssign(const AssignExprAST* stmt);

    /// @brief Execute an expression statement.
    ConstantValue executeExprStmt(const ExprStmtAST* stmt);

    /// @brief Execute a break statement.
    ConstantValue executeBreak();

    /// @brief Execute a continue statement.
    ConstantValue executeContinue();

    /// @brief Execute a declaration statement.
    ConstantValue executeDeclStmt(const DeclStmtAST* stmt);

    // ─── Expression Evaluation ────────────────────────────────────────

    /// @brief Evaluate an expression in the current context.
    ConstantValue evalExpr(const ExprAST* expr);

    /// @brief Evaluate an identifier (local variable or const).
    ConstantValue evalIdentifier(const IdentifierExprAST* expr);

    /// @brief Evaluate a binary expression.
    ConstantValue evalBinary(const BinaryExprAST* expr);

    /// @brief Evaluate a unary expression.
    ConstantValue evalUnary(const UnaryExprAST* expr);

    /// @brief Evaluate a call expression (nested const function call).
    ConstantValue evalCall(const CallExprAST* expr);

    // ─── Helpers ──────────────────────────────────────────────────────

    /// @brief Get a local variable value.
    ConstantValue getLocal(InternedString name) const;

    /// @brief Set a local variable value.
    void setLocal(InternedString name, const ConstantValue& value);

    /// @brief Check if a block always returns.
    bool blockAlwaysReturns(const BlockStmtAST* block) const;

    /// @brief Check if a statement is pure (no side effects).
    bool isPureStmt(const StmtAST* stmt) const;

    /// @brief Report an error during interpretation.
    void reportError(const BaseAST* node, const std::string& msg);
};

} // namespace sema