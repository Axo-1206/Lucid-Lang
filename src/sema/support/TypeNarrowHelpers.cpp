/// @file TypeNarrowHelpers.cpp
/// @brief Implementation of type narrowing helper functions.

#include "TypeNarrowHelpers.hpp"
#include "../Sema.hpp"
#include "../types/SemaCompare.hpp"
#include "../types/SemaResolve.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// extractNarrowingsFromCondition
// ─────────────────────────────────────────────────────────────────────────────

NarrowingInfo extractNarrowingsFromCondition(ExprAST* expr, SemaContext& ctx,
                                               bool* outIsValidMixed) {
    NarrowingInfo result;
    result.hasNarrowing = false;

    if (outIsValidMixed) *outIsValidMixed = true;

    if (!expr) return result;

    // ─── 1. Handle `or` at top level ─────────────────────────────────────
    // Pattern: a == nil or b == nil
    if (expr->isa<BinaryExprAST>() && expr->as<BinaryExprAST>()->op == BinaryOp::Or) {
        BinaryExprAST* binary = expr->as<BinaryExprAST>();
        
        bool leftMixed = false;
        bool rightMixed = false;
        NarrowingInfo left = extractNarrowingsFromCondition(binary->left, ctx, &leftMixed);
        NarrowingInfo right = extractNarrowingsFromCondition(binary->right, ctx, &rightMixed);
        
        // Check for mixed operators - reject if either side has mixed operators
        if (leftMixed || rightMixed) {
            if (outIsValidMixed) *outIsValidMixed = false;
            return NarrowingInfo();
        }
        
        // Check operator consistency when both sides have narrowing
        if (left.hasNarrowing && right.hasNarrowing) {
            if (left.isEquality != right.isEquality) {
                if (outIsValidMixed) *outIsValidMixed = false;
                return NarrowingInfo();
            }
        }
        
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
            if (!left.hasNarrowing) {
                result.isEquality = right.isEquality;
            }
            for (const auto& [name, type] : right.narrowings) {
                result.narrowings[name] = type;
            }
        }
        
        // If only right has narrowing, use its isEquality
        if (right.hasNarrowing && !left.hasNarrowing) {
            result.isEquality = right.isEquality;
        }
        
        return result;
    }
    
    // ─── 2. Handle `and` at top level ─────────────────────────────────────
    // Pattern: a == nil and b == nil → No narrowing (unsound)
    if (expr->isa<BinaryExprAST>() && expr->as<BinaryExprAST>()->op == BinaryOp::And) {
        // No narrowing applied when 'and' is at the top level
        // This is unsound because inverse would be OR, not AND
        return NarrowingInfo();
    }
    
    // ─── 3. Handle simple binary comparison ──────────────────────────────
    if (expr->isa<BinaryExprAST>()) {
        return detectSingleNarrowing(expr->as<BinaryExprAST>(), ctx);
    }

    // ─── 4. Handle `not x` ──────────────────────────────────────────────
    // Pattern: not x → x is nil/false (inverse narrowing)
    if (expr->isa<UnaryExprAST>() && expr->as<UnaryExprAST>()->op == UnaryOp::Not) {
        const UnaryExprAST* unary = expr->as<UnaryExprAST>();
        if (unary->operand->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = unary->operand->as<IdentifierExprAST>();
            
            ValueDeclAST* decl = ctx.lookupValue(id->name);
            if (decl) {
                TypeAST* innerType = getInnerType(decl, ctx);
                if (innerType) {
                    result.hasNarrowing = true;
                    // `not x` is treated as equality for inverse narrowing
                    // i.e., if `not x` is true, x is nil/false
                    result.isEquality = true;
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

NarrowingInfo detectSingleNarrowing(BinaryExprAST* binary, SemaContext& ctx) {
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
        IdentifierExprAST* id = binary->left->as<IdentifierExprAST>();
        const LiteralExprAST* lit = binary->right->as<LiteralExprAST>();

        detectIdentifierNarrowing(result, id, lit, isEquality, ctx);
        return result;
    }

    // Also check reverse: (nil == identifier) or (err == identifier)
    if (binary->left->isa<LiteralExprAST>() && binary->right->isa<IdentifierExprAST>()) {
        const LiteralExprAST* lit = binary->left->as<LiteralExprAST>();
        IdentifierExprAST* id = binary->right->as<IdentifierExprAST>();

        detectIdentifierNarrowing(result, id, lit, isEquality, ctx);
        return result;
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// detectIdentifierNarrowing
// ─────────────────────────────────────────────────────────────────────────────

void detectIdentifierNarrowing(NarrowingInfo& info, IdentifierExprAST* id, 
                                 const LiteralExprAST* lit, bool isEquality, 
                                 SemaContext& ctx) {
    if (!id || !lit) return;

    // Check if literal is nil or err
    if (lit->kind != LiteralKind::Nil && lit->kind != LiteralKind::Err) {
        return;
    }

    // Look up the variable using existing infrastructure
    ValueDeclAST* decl = ctx.lookupValue(id->name);
    if (!decl) return;

    // Check if the variable is nullable or fallible using SemaCompare
    if (!isNullableType(decl->type) && !isFallibleType(decl->type)) {
        return;
    }

    TypeAST* innerType = getInnerType(decl, ctx);
    if (!innerType) return;

    info.hasNarrowing = true;
    info.isEquality = isEquality;
    info.narrowings[id->name] = innerType;
}

// ─────────────────────────────────────────────────────────────────────────────
// getInnerType
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* getInnerType(ValueDeclAST* decl, SemaContext& ctx) {
    if (!decl || !decl->type) return nullptr;

    TypeAST* type = decl->type;

    // Unwrap nullable using SemaCompare
    if (isNullableType(type)) {
        type = unwrapNullable(type);
    }

    // Unwrap fallible using SemaCompare
    if (isFallibleType(type)) {
        type = unwrapFallible(type);
    }

    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// detectNarrowingPattern
// ─────────────────────────────────────────────────────────────────────────────

NarrowingInfo detectNarrowingPattern(BinaryExprAST* binary, SemaContext& ctx) {
    NarrowingInfo result;
    result.hasNarrowing = false;

    if (!binary) return result;

    // Delegate to the main extraction function
    bool isMixed = false;
    result = extractNarrowingsFromCondition(binary, ctx, &isMixed);

    // If mixed operators were detected, report an error
    if (isMixed) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, binary,
                              "mixed '==' and '!=' in condition for type narrowing");
        return NarrowingInfo();
    }

    // Additional validation: check that the variables being narrowed are
    // actually defined and have the expected type
    if (result.hasNarrowing) {
        for (const auto& [varName, narrowedType] : result.narrowings) {
            // Look up the variable using existing infrastructure
            ValueDeclAST* decl = ctx.lookupValue(varName);
            if (!decl) {
                ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, binary,
                                      "undefined variable '", ctx.pool.lookup(varName), "'");
                return NarrowingInfo();
            }

            // Verify the variable is nullable or fallible using SemaCompare
            if (!isNullableType(decl->type) && !isFallibleType(decl->type)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidNilCheck, binary,
                                      "cannot narrow non-nullable/non-fallible variable '",
                                      ctx.pool.lookup(varName), "'");
                return NarrowingInfo();
            }

            // Verify the narrowed type is valid
            if (!narrowedType) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, binary,
                                      "cannot narrow variable '", ctx.pool.lookup(varName),
                                      "' to invalid type");
                return NarrowingInfo();
            }
        }

        // Verify that we're not trying to narrow the same variable twice
        std::unordered_set<InternedString> seenVars;
        for (const auto& [varName, _] : result.narrowings) {
            if (seenVars.find(varName) != seenVars.end()) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidNilCheck, binary,
                                      "duplicate narrowing for variable '",
                                      ctx.pool.lookup(varName), "'");
                return NarrowingInfo();
            }
            seenVars.insert(varName);
        }
    }

    return result;
}

} // namespace sema