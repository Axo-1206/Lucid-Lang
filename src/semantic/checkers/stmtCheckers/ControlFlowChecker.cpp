/**
 * @file ControlFlowChecker.cpp
 * @brief Semantic checking for control flow statements: if, return, break, continue.
 */

#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// If Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkIfStmt(IfStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (!condType) return;

    if (!TypeChecker::isBooleanCompatible(condType, ctx)) {
        ctx.error(node.condition->loc, DiagCode::E2002, "if condition must be boolean");
        return;
    }

    if (node.thenBranch) {
        checkStmt(node.thenBranch.get(), ctx, expectedReturn);
    }

    if (node.elseBranch) {
        checkStmt(node.elseBranch.get(), ctx, expectedReturn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Return Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkReturnStmt(ReturnStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    if (node.values.empty()) {
        // Bare return
        if (expectedReturn != nullptr) {
            ctx.error(node.loc, DiagCode::E2002, "bare return in non-void function");
        }
        return;
    }

    // Currently only support single return value
    if (node.values.size() == 1) {
        TypeAST* retType = checkExpr(node.values[0].get(), ctx);
        if (!retType) return;
        
        if (expectedReturn == nullptr) {
            ctx.error(node.loc, DiagCode::E2002, "return value in void function");
            return;
        }
        
        if (!TypeChecker::isAssignable(retType, expectedReturn, ctx)) {
            ctx.error(node.values[0]->loc, DiagCode::E2002,
                      "return type mismatch: expected ",
                      LucDebug::kindToString(expectedReturn->kind), ", got ",
                      LucDebug::kindToString(retType->kind));
        }
    } else {
        ctx.error(node.loc, DiagCode::E2002, "multiple return values not yet fully supported");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Break Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkBreakStmt(BreakStmtAST& node, SemanticContext& ctx) {
    if (ctx.loopDepth == 0) {
        ctx.error(node.loc, DiagCode::E2027, "break statement outside loop");
    }
    if (ctx.parallelDepth > 0) {
        ctx.error(node.loc, DiagCode::E2027, "break not allowed inside parallel block or parallel for");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Continue Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkContinueStmt(ContinueStmtAST& node, SemanticContext& ctx) {
    if (ctx.loopDepth == 0) {
        ctx.error(node.loc, DiagCode::E2027, "continue statement outside loop");
    }
    if (ctx.parallelDepth > 0) {
        ctx.error(node.loc, DiagCode::E2027, "continue not allowed inside parallel block or parallel for");
    }
}