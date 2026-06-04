/**
 * @file ExprDispatcher.cpp
 * @brief Dispatcher for expression checking (Phase 3b).
 *
 * Routes each expression node to its specific checker based on ASTKind.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// checkExpr — Main dispatcher
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx) {
    if (!node) return nullptr;
    
    // Return cached type if already resolved
    if (node->resolvedType) return static_cast<TypeAST*>(node->resolvedType);

    TypeAST* result = nullptr;
    
    switch (node->kind) {
        // ── Literal Expressions ─────────────────────────────────────────────
        case ASTKind::LiteralExpr:
            result = checkLiteralExpr(*node->as<LiteralExprAST>(), ctx);
            break;
        case ASTKind::ArrayLiteralExpr:
            result = checkArrayLiteralExpr(*node->as<ArrayLiteralExprAST>(), ctx);
            break;
        case ASTKind::StructLiteralExpr:
            result = checkStructLiteralExpr(*node->as<StructLiteralExprAST>(), ctx);
            break;
        case ASTKind::AnonFuncExpr:
            result = checkAnonFuncExpr(*node->as<AnonFuncExprAST>(), ctx);
            break;

        // ── Operator Expressions ───────────────────────────────────────────
        case ASTKind::BinaryExpr:
            result = checkBinaryExpr(*node->as<BinaryExprAST>(), ctx);
            break;
        case ASTKind::UnaryExpr:
            result = checkUnaryExpr(*node->as<UnaryExprAST>(), ctx);
            break;
        case ASTKind::AssignExpr:
            result = checkAssignExpr(*node->as<AssignExprAST>(), ctx);
            break;
        case ASTKind::IsExpr:
            result = checkIsExpr(*node->as<IsExprAST>(), ctx);
            break;
        case ASTKind::NullCoalesceExpr:
            result = checkNullCoalesceExpr(*node->as<NullCoalesceExprAST>(), ctx);
            break;
        case ASTKind::PipelineExpr:
            result = checkPipelineExpr(*node->as<PipelineExprAST>(), ctx);
            break;
        case ASTKind::ComposeExpr:
            result = checkComposeExpr(*node->as<ComposeExprAST>(), ctx);
            break;

        // ── Special Expressions ────────────────────────────────────────────
        case ASTKind::AwaitExpr:
            result = checkAwaitExpr(*node->as<AwaitExprAST>(), ctx);
            break;
        case ASTKind::IfExpr:
            result = checkIfExpr(*node->as<IfExprAST>(), ctx);
            break;
        case ASTKind::IntrinsicCallExpr:
            result = checkIntrinsicCallExpr(*node->as<IntrinsicCallExprAST>(), ctx);
            break;
        case ASTKind::RangeExpr:
            result = checkRangeExpr(*node->as<RangeExprAST>(), ctx);
            break;
        case ASTKind::TypeConvExpr:
            result = checkTypeConvExpr(*node->as<TypeConvExprAST>(), ctx);
            break;
        case ASTKind::NullableChainExpr:
            result = checkNullableChainExpr(*node->as<NullableChainExprAST>(), ctx);
            break;

        // ── Other Expressions ──────────────────────────────────────────────
        case ASTKind::CallExpr:
            result = checkCallExpr(*node->as<CallExprAST>(), ctx);
            break;
        case ASTKind::IndexExpr:
            result = checkIndexExpr(*node->as<IndexExprAST>(), ctx);
            break;
        case ASTKind::FieldAccessExpr:
            result = checkFieldAccessExpr(*node->as<FieldAccessExprAST>(), ctx);
            break;
        case ASTKind::BehaviorAccessExpr:
            result = checkBehaviorAccessExpr(*node->as<BehaviorAccessExprAST>(), ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = checkIdentifierExpr(*node->as<IdentifierExprAST>(), ctx);
            break;

        // ── Match Expressions ──────────────────────────────────────────────
        case ASTKind::MatchExpr:
            result = checkMatchExpr(*node->as<MatchExprAST>(), ctx);
            break;

        default:
            ctx.error(node->loc, DiagCode::E2002,
                      "unsupported expression kind: ", LucDebug::kindToString(node->kind));
            return nullptr;
    }

    node->resolvedType = result;
    return result;
}