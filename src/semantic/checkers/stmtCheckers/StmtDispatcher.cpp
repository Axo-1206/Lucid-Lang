/**
 * @file StmtDispatcher.cpp
 *
 * @responsibility Dispatcher for statement checking (Phase 3c).
 */

#include "ast/StmtAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// Statement checkers are defined in separate files:
// - BlockChecker.cpp
// - DeclStmtChecker.cpp
// - ExprStmtChecker.cpp
// - ControlFlowChecker.cpp (if, return, break, continue)
// - LoopChecker.cpp (for, while, do-while)
// - SwitchChecker.cpp
// - MultiAssignChecker.cpp
// - MultiVarDeclChecker.cpp

void checkStmt(StmtAST* node, SemanticContext& ctx, TypeAST* expectedReturn) {
    if (!node) return;

    switch (node->kind) {
        case ASTKind::BlockStmt:
            checkBlockStmt(*node->as<BlockStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ExprStmt:
            checkExprStmt(*node->as<ExprStmtAST>(), ctx);
            break;
        case ASTKind::DeclStmt:
            checkDeclStmt(*node->as<DeclStmtAST>(), ctx);
            break;
        case ASTKind::IfStmt:
            checkIfStmt(*node->as<IfStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::WhileStmt:
            checkWhileStmt(*node->as<WhileStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ForStmt:
            checkForStmt(*node->as<ForStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::DoWhileStmt:
            checkDoWhileStmt(*node->as<DoWhileStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ReturnStmt:
            checkReturnStmt(*node->as<ReturnStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::BreakStmt:
            checkBreakStmt(*node->as<BreakStmtAST>(), ctx);
            break;
        case ASTKind::ContinueStmt:
            checkContinueStmt(*node->as<ContinueStmtAST>(), ctx);
            break;
        case ASTKind::SwitchStmt:
            checkSwitchStmt(*node->as<SwitchStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::MultiVarDecl:
            checkMultiVarDecl(*node->as<MultiVarDeclAST>(), ctx);
            break;
        case ASTKind::MultiAssignStmt:
            checkMultiAssignStmt(*node->as<MultiAssignStmtAST>(), ctx);
            break;
        default:
            ctx.error(node->loc, DiagCode::E2002,
                      "unsupported statement kind: ", LucDebug::kindToString(node->kind));
            break;
    }
}