/// @file TypeNarrowHelper.cpp
/// @brief Implementation of type narrowing helper functions.
/// 
/// These functions extract narrowing information from condition expressions
/// to enable type narrowing in if statements.

#include "TypeNarrowHelpers.hpp"
#include "../Sema.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// extractNarrowingsFromCondition
// ─────────────────────────────────────────────────────────────────────────────

NarrowingInfo extractNarrowingsFromCondition(const ExprAST* expr, SemaContext& ctx) {
    NarrowingInfo result;
    result.hasNarrowing = false;

    if (!expr) return result;

    // ─── 1. Handle `or` at top level ─────────────────────────────────────
    // Pattern: a == nil or b == nil
    if (expr->isa<BinaryExprAST>() && expr->as<BinaryExprAST>()->op == BinaryOp::Or) {
        const BinaryExprAST* binary = expr->as<BinaryExprAST>();
        
        NarrowingInfo left = extractNarrowingsFromCondition(binary->left, ctx);
        NarrowingInfo right = extractNarrowingsFromCondition(binary->right, ctx);
        
        // Merge both narrowings
        if (left.hasNarrowing) {
            result.hasNarrowing = true;
            result.isEquality = left.isEquality;
            for (const auto& [name, type] : left.narrowings) {
                result.narrowings[name] = type;
            }
        }
        if (right.hasNarrowing) {
            result.hasNarrowing = true;
            // Keep isEquality from left if both have narrowing
            for (const auto& [name, type] : right.narrowings) {
                result.narrowings[name] = type;
            }
        }
        return result;
    }
    
    // ─── 2. Handle `and` at top level ─────────────────────────────────────
    // Pattern: a == nil and b == nil → No narrowing (unsound)
    if (expr->isa<BinaryExprAST>() && expr->as<BinaryExprAST>()->op == BinaryOp::And) {
        // No narrowing applied when 'and' is at the top level
        return result;
    }
    
    // ─── 3. Handle simple binary comparison ──────────────────────────────
    if (expr->isa<BinaryExprAST>()) {
        return detectSingleNarrowing(expr->as<BinaryExprAST>(), ctx);
    }

    // ─── 4. Handle `not a` ──────────────────────────────────────────────
    // Pattern: not x → x is nil/false (inverse narrowing)
    if (expr->isa<UnaryExprAST>() && expr->as<UnaryExprAST>()->op == UnaryOp::Not) {
        const UnaryExprAST* unary = expr->as<UnaryExprAST>();
        if (unary->operand->isa<IdentifierExprAST>()) {
            const IdentifierExprAST* id = unary->operand->as<IdentifierExprAST>();
            
            // Get the inner type
            const ValueDeclAST* decl = lookupValue(id->name, ctx);
            if (decl) {
                const TypeAST* innerType = getInnerType(decl, ctx);
                if (innerType) {
                    result.hasNarrowing = true;
                    result.isEquality = true; // Treat as equality for inverse narrowing
                    result.narrowings[id->name] = innerType;
                }
            }
            return result;
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// detectSingleNarrowing
// ─────────────────────────────────────────────────────────────────────────────

NarrowingInfo detectSingleNarrowing(const BinaryExprAST* binary, SemaContext& ctx) {
    NarrowingInfo result;
    result.hasNarrowing = false;

    if (!binary) return result;

    // Only detect for equality/inequality operators
    if (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne) {
        return result;
    }

    bool isEquality = (binary->op == BinaryOp::Eq);

    // Pattern: (identifier == nil) or (identifier != nil)
    // Pattern: (identifier == err) or (identifier != err)
    if (binary->left->isa<IdentifierExprAST>() && binary->right->isa<LiteralExprAST>()) {
        const IdentifierExprAST* id = binary->left->as<IdentifierExprAST>();
        const LiteralExprAST* lit = binary->right->as<LiteralExprAST>();

        detectIdentifierNarrowing(result, id, lit, isEquality, ctx);
        return result;
    }

    // Also check reverse: (nil == identifier) or (err == identifier)
    if (binary->left->isa<LiteralExprAST>() && binary->right->isa<IdentifierExprAST>()) {
        const LiteralExprAST* lit = binary->left->as<LiteralExprAST>();
        const IdentifierExprAST* id = binary->right->as<IdentifierExprAST>();

        detectIdentifierNarrowing(result, id, lit, isEquality, ctx);
        return result;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// detectIdentifierNarrowing
// ─────────────────────────────────────────────────────────────────────────────

void detectIdentifierNarrowing(NarrowingInfo& info, const IdentifierExprAST* id, 
                                 const LiteralExprAST* lit, bool isEquality, 
                                 SemaContext& ctx) {
    if (!id || !lit) return;

    // Check if literal is nil or err
    if (lit->kind != LiteralKind::Nil && lit->kind != LiteralKind::Err) {
        return;
    }

    // Look up the variable
    const ValueDeclAST* decl = lookupValue(id->name, ctx);
    if (!decl) return;

    // Check if the variable is nullable or fallible
    if (!isNullableType(decl->type) && !isFallibleType(decl->type)) {
        return;
    }

    const TypeAST* innerType = getInnerType(decl, ctx);
    if (!innerType) return;

    info.hasNarrowing = true;
    info.isEquality = isEquality;
    info.narrowings[id->name] = innerType;
}

// ─────────────────────────────────────────────────────────────────────────────
// getInnerType
// ─────────────────────────────────────────────────────────────────────────────

const TypeAST* getInnerType(const ValueDeclAST* decl, SemaContext& ctx) {
    if (!decl || !decl->type) return nullptr;

    const TypeAST* type = decl->type;

    // Unwrap nullable
    if (isNullableType(type)) {
        type = unwrapNullable(const_cast<TypeAST*>(type));
    }

    // Unwrap fallible
    if (isFallibleType(type)) {
        type = unwrapFallible(const_cast<TypeAST*>(type));
    }

    return type;
}

} // namespace sema