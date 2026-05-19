/**
 * @file SemanticExpr.cpp
 *
 * @nutshell Validates exactly how expressions interact with each other in code.
 *
 * @responsibility Phase 3b of semantic analysis: walks every expression node, resolves
 *   its type, writes resolvedType onto the node, and enforces all operator and
 *   language-level expression rules.
 *
 * @logic
 *   checkExpr dispatches to a handler per ASTKind. Each handler returns the TypeAST*
 *   that the expression evaluates to, or nullptr if an irrecoverable error occurred.
 *   The returned pointer is also written onto node->resolvedType for the Annotator.
 *
 * @related SemanticAnalyzer.cpp, SemanticDecl.cpp, SemanticStmt.cpp
 */

#include "registry/IntrinsicRegistry.hpp"
#include "registry/BuiltinMethodRegistry.hpp"
#include "ast/BaseAST.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "header/SymbolTable.hpp"
#include "header/TypeResolver.hpp"
#include "header/SemanticContext.hpp"

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& loopDepth, int& parallelDepth, bool insideExtern);


TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx) {
    if (!node) return nullptr;

    switch (node->kind) {
        case ASTKind::LiteralExpr:
            return checkLiteralExpr(*node->as<LiteralExprAST>(), ctx);
        case ASTKind::IdentifierExpr:
            return checkIdentifierExpr(*node->as<IdentifierExprAST>(), ctx);
        case ASTKind::BinaryExpr:
            return checkBinaryExpr(*node->as<BinaryExprAST>(), ctx);
        case ASTKind::UnaryExpr:
            return checkUnaryExpr(*node->as<UnaryExprAST>(), ctx);
        case ASTKind::CallExpr:
            return checkCallExpr(*node->as<CallExprAST>(), ctx);
        case ASTKind::IndexExpr:
            return checkIndexExpr(*node->as<IndexExprAST>(), ctx);
        case ASTKind::FieldAccessExpr:
            return checkFieldAccessExpr(*node->as<FieldAccessExprAST>(), ctx);
        case ASTKind::StructLiteralExpr:
            return checkStructLiteralExpr(*node->as<StructLiteralExprAST>(), ctx);
        case ASTKind::ArrayLiteralExpr:
            return checkArrayLiteralExpr(*node->as<ArrayLiteralExprAST>(), ctx);
        case ASTKind::TypeConvExpr:
            return checkTypeConvExpr(*node->as<TypeConvExprAST>(), ctx);
        // ... add other nodes as needed (IfExpr, MatchExpr, AwaitExpr, etc.)
        default:
            ctx.dc.error(DiagnosticCategory::Semantic, node->loc, DiagCode::E3002,
                         "unsupported expression kind");
            return nullptr;
    }
}