/**
 * @file LiteralChecker.cpp
 * @brief Semantic checking for scalar literal expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkLiteralExpr(LiteralExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkLiteralExpr: kind=" << static_cast<int>(node.kind));
    
    TypeAST* result = nullptr;
    
    switch (node.kind) {
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int).release();
            break;
            
        case LiteralKind::Float:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Float).release();
            break;
            
        case LiteralKind::String:
        case LiteralKind::RawString:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::String).release();
            break;
            
        case LiteralKind::Char:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Char).release();
            break;
            
        case LiteralKind::True:
        case LiteralKind::False:
            result = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).release();
            node.isConst = true;
            break;
            
        case LiteralKind::Nil:
            // nil has no type by itself; will be unified later in context
            result = nullptr;
            break;
            
        default:
            ctx.error(node.loc, DiagCode::E2002, "invalid literal kind");
            return nullptr;
    }
    
    // Literals (except nil) are compile-time constants
    if (result != nullptr) {
        node.isConst = true;
    }
    
    return result;
}