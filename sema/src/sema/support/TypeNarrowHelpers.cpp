/// @file TypeNarrowHelpers.cpp
/// @brief Implementation of narrowing detection.

#include "TypeNarrowHelpers.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaType.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The "inner" type of a `T?` — the type a narrowed `T?` value becomes.
/// For a non-nullable type, returns the type itself; narrowing such a
/// value is a no-op and the caller uses the type unchanged.
TypeAST* innerTypeOf(TypeAST* type) {
    if (!type) return nullptr;
    if (isNullableType(type)) return unwrapNullable(type);
    return type;
}

/// True if `decl` refers to a value whose type is nullable.
bool isNullableValue(const ValueDeclAST* decl) {
    return decl && decl->type && isNullableType(decl->type);
}

/// Record a narrowing in `info`. Sets `hasNarrowing` and inserts the
/// entry, but leaves `isEquality` alone — the caller is responsible for
/// setting it once, after all narrowings in the condition have been
/// merged and their operator has been confirmed consistent.
void addNarrowing(NarrowingInfo& info, InternedString name, TypeAST* type) {
    info.hasNarrowing = true;
    info.narrowings[name] = type;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// detectNarrowingPattern
// ─────────────────────────────────────────────────────────────────────────────

NarrowingInfo detectNarrowingPattern(BinaryExprAST* binary, SemaContext& ctx) {
    NarrowingInfo result;
    result.hasNarrowing = false;

    if (!binary) return result;

    // Delegate to the general extraction. `outMixed` is set to `true`
    // only when the condition mixes `==` and `!=` in a way that makes
    // narrowing unsound; in every other case it stays `false`.
    bool outMixed = false;
    result = extractNarrowingsFromCondition(binary, ctx, &outMixed);

    if (outMixed) {
        ctx.diagnostics.error(DiagCode::Type_InvalidBinary, binary,
                              "condition mixes '==' and '!=' in a way that "
                              "prevents type narrowing; rewrite the "
                              "condition with a single operator");
        return NarrowingInfo();
    }

    // ─── Validate each detected narrowing ───────────────────────────────
    //
    // The extractor records a name → type pair for every identifier
    // that looked like a narrowing site. But not every such identifier
    // is a genuine narrowing: the identifier might not resolve, or it
    // might refer to a non-nullable value (in which case narrowing is a
    // no-op, not an error).
    //
    // The check walks the resolved narrowings and drops any that do not
    // correspond to a nullable value. A narrowing of a non-nullable
    // value is silently dropped — `if x != nil` where `x: int` is a
    // type error at a different point (comparing a non-nullable to
    // `nil`), and the narrowing detector is not the right place to
    // report it. The comparison itself will fail its type check.
    NarrowingInfo validated;
    validated.isEquality = result.isEquality;
    for (const auto& [name, narrowedType] : result.narrowings) {
        ValueDeclAST* decl = ctx.lookupValue(name);
        if (!decl) continue;
        if (!isNullableValue(decl)) continue;
        if (!narrowedType) continue;
        addNarrowing(validated, name, narrowedType);
    }

    return validated;
}

// ─────────────────────────────────────────────────────────────────────────────
// extractSingleBinaryNarrowing
// ─────────────────────────────────────────────────────────────────────────────
//
// A single `x == nil` / `x != nil` binary. The identifier can be on
// either side; the literal can be on either side. Both orders are
// recognized.

static NarrowingInfo extractSingleBinaryNarrowing(BinaryExprAST* binary,
                                                  SemaContext& ctx) {
    NarrowingInfo result;
    result.hasNarrowing = false;
    if (!binary) return result;

    // Only `==` and `!=` produce narrowings. Everything else (`<`,
    // `and`, `+`, `??`) is not a narrowing site.
    if (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne) {
        return result;
    }

    const bool isEquality = (binary->op == BinaryOp::Eq);

    // ─── One side must be an identifier, the other a `nil` literal ──────
    //
    // Two orders:
    //   - `x == nil`, `x != nil`
    //   - `nil == x`, `nil != x`
    IdentifierExprAST* id = nullptr;
    LiteralExprAST*    lit = nullptr;

    if (binary->left->isa<IdentifierExprAST>() &&
        binary->right->isa<LiteralExprAST>()) {
        id  = binary->left->as<IdentifierExprAST>();
        lit = binary->right->as<LiteralExprAST>();
    } else if (binary->left->isa<LiteralExprAST>() &&
               binary->right->isa<IdentifierExprAST>()) {
        id  = binary->right->as<IdentifierExprAST>();
        lit = binary->left->as<LiteralExprAST>();
    } else {
        return result;
    }

    if (lit->kind != LiteralKind::Nil) return result;

    // ─── The identifier must refer to a nullable value ──────────────────
    ValueDeclAST* decl = ctx.lookupValue(id->name);
    if (!decl || !isNullableValue(decl)) {
        return result;
    }

    TypeAST* inner = innerTypeOf(decl->type);
    if (!inner) return result;

    result.hasNarrowing = true;
    result.isEquality   = isEquality;
    addNarrowing(result, id->name, inner);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// extractNarrowingsFromCondition
// ─────────────────────────────────────────────────────────────────────────────

NarrowingInfo extractNarrowingsFromCondition(ExprAST* expr, SemaContext& ctx,
                                             bool* outMixed) {
    NarrowingInfo result;
    result.hasNarrowing = false;
    if (outMixed) *outMixed = false;
    if (!expr) return result;

    // ─── `and` chains ───────────────────────────────────────────────────
    //
    // `x != nil and y != nil` narrows both. `x == nil and y == nil`
    // narrows neither in the then-branch (the conjunction is true only
    // when *both* are nil, so neither is non-nil in the then). The
    // extraction recurses into both sides and merges; a mixed operator
    // (`!=` on one side, `==` on the other) is rejected.
    if (expr->isa<BinaryExprAST>() &&
        expr->as<BinaryExprAST>()->op == BinaryOp::And) {
        BinaryExprAST* andExpr = expr->as<BinaryExprAST>();

        bool leftMixed  = false;
        bool rightMixed = false;
        NarrowingInfo left  = extractNarrowingsFromCondition(andExpr->left,  ctx, &leftMixed);
        NarrowingInfo right = extractNarrowingsFromCondition(andExpr->right, ctx, &rightMixed);

        if (leftMixed || rightMixed) {
            if (outMixed) *outMixed = true;
            return NarrowingInfo();
        }

        // Operator consistency: both sides must use `!=` for the
        // narrowing to apply in the then-branch. An `and` of `x == nil`
        // and `y == nil` is a single condition that is true only when
        // both are nil — the then-branch is the "both nil" case, not a
        // narrowing of either. So a `==`-based `and` produces no
        // narrowing at all, and a `!=`-based `and` produces narrowings
        // for every operand.
        if (left.hasNarrowing && left.isEquality) {
            // `==` in an `and` — no then-branch narrowing.
            return NarrowingInfo();
        }
        if (right.hasNarrowing && right.isEquality) {
            return NarrowingInfo();
        }

        // Merge: both sides narrow, and the merged result uses `!=`
        // (isEquality = false).
        if (left.hasNarrowing) {
            result.hasNarrowing = true;
            for (const auto& [name, type] : left.narrowings) {
                addNarrowing(result, name, type);
            }
        }
        if (right.hasNarrowing) {
            result.hasNarrowing = true;
            for (const auto& [name, type] : right.narrowings) {
                addNarrowing(result, name, type);
            }
        }
        result.isEquality = false;
        return result;
    }

    // ─── `or` chains ────────────────────────────────────────────────────
    //
    // `x == nil or y == nil` narrows both in the *else*-branch: the
    // else-branch runs when both are non-nil. This is inverse
    // narrowing — the operator is `==`, and the narrowing applies to
    // the else.
    //
    // `x != nil or y != nil` does not narrow in either branch: the
    // then-branch runs when at least one is non-nil (nothing to say
    // about the other), and the else-branch runs when both are nil
    // (no non-nil type to assign).
    if (expr->isa<BinaryExprAST>() &&
        expr->as<BinaryExprAST>()->op == BinaryOp::Or) {
        BinaryExprAST* orExpr = expr->as<BinaryExprAST>();

        bool leftMixed  = false;
        bool rightMixed = false;
        NarrowingInfo left  = extractNarrowingsFromCondition(orExpr->left,  ctx, &leftMixed);
        NarrowingInfo right = extractNarrowingsFromCondition(orExpr->right, ctx, &rightMixed);

        if (leftMixed || rightMixed) {
            if (outMixed) *outMixed = true;
            return NarrowingInfo();
        }

        // Only `==`-based `or` produces a narrowing, and only in the
        // else-branch. A `!=`-based `or` produces no narrowing.
        if ((left.hasNarrowing && !left.isEquality) ||
            (right.hasNarrowing && !right.isEquality)) {
            return NarrowingInfo();
        }

        if (left.hasNarrowing) {
            result.hasNarrowing = true;
            for (const auto& [name, type] : left.narrowings) {
                addNarrowing(result, name, type);
            }
        }
        if (right.hasNarrowing) {
            result.hasNarrowing = true;
            for (const auto& [name, type] : right.narrowings) {
                addNarrowing(result, name, type);
            }
        }
        result.isEquality = true;   // `==`-based; applies to else
        return result;
    }

    // ─── Single comparison ──────────────────────────────────────────────
    if (expr->isa<BinaryExprAST>()) {
        return extractSingleBinaryNarrowing(expr->as<BinaryExprAST>(), ctx);
    }

    // ─── `not x` ────────────────────────────────────────────────────────
    //
    // `not x` where `x: int?` is true when `x` is nil. This is
    // equivalent to `x == nil`: the inverse narrowing (x is non-nil)
    // applies to the else-branch.
    //
    // The check reads the operand's shape — an identifier — and looks
    // up the identifier's type. A `not` on a non-identifier (a call, a
    // binary, a field access) does not produce a narrowing; the
    // grammar's narrowing is only over named bindings.
    if (expr->isa<UnaryExprAST>() &&
        expr->as<UnaryExprAST>()->op == UnaryOp::Not) {
        UnaryExprAST* unary = expr->as<UnaryExprAST>();
        if (unary->operand && unary->operand->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = unary->operand->as<IdentifierExprAST>();
            ValueDeclAST* decl = ctx.lookupValue(id->name);
            if (decl && isNullableValue(decl)) {
                TypeAST* inner = innerTypeOf(decl->type);
                if (inner) {
                    result.hasNarrowing = true;
                    result.isEquality = true;
                    addNarrowing(result, id->name, inner);
                }
            }
        }
        return result;
    }

    return result;
}


} // namespace lucid::sema