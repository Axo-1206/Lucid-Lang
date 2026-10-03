/// @file SemaTypeEquality.cpp
/// @brief Type equality and assignability.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/TypeAST.hpp"

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// typesEqual
// ─────────────────────────────────────────────────────────────────────────────

bool typesEqual(TypeAST* a, TypeAST* b) {
    // ─── Fast path: pointer equality ───────────────────────────────────
    //
    // The TypeCache canonicalizes every type it produces, so two types
    // with the same structure share a node in the common case. This is
    // the path that runs for `int == int`, `[int] == [int]`, and the
    // other cases where both sides came through the cache.
    if (a == b) return true;

    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case ASTKind::PrimitiveType:
            return a->as<PrimitiveTypeAST>()->primitiveKind
                == b->as<PrimitiveTypeAST>()->primitiveKind;

        case ASTKind::NamedType: {
            NamedTypeAST* na = a->as<NamedTypeAST>();
            NamedTypeAST* nb = b->as<NamedTypeAST>();

            // ─── Resolved on both sides: compare the declarations ──────
            //
            // A NamedTypeAST's identity is its resolved TableDeclAST,
            // not its source spelling. `weapons.Item` and `consumables.Item`
            // are different types even though both are named "Item" (§5).
            // Their resolvedDecls are different, so pointer comparison
            // says "not equal" — the correct answer.
            if (na->resolvedDecl && nb->resolvedDecl) {
                return na->resolvedDecl == nb->resolvedDecl;
            }

            // ─── At least one side unresolved: compare by name ─────────
            //
            // This path runs when one side is a freshly-parsed type
            // annotation that hasn't been through resolveType. Comparing
            // by (qualifier, name) is the best that can be done without
            // a resolvedDecl to anchor on.
            return na->qualifier == nb->qualifier
                && na->name      == nb->name;
        }

        case ASTKind::NullableType:
            return typesEqual(a->as<NullableTypeAST>()->inner,
                              b->as<NullableTypeAST>()->inner);

        case ASTKind::ArrayType: {
            ArrayTypeAST* aa = a->as<ArrayTypeAST>();
            ArrayTypeAST* ab = b->as<ArrayTypeAST>();
            if (aa->arrayKind != ab->arrayKind) return false;
            if (aa->fixedSize != ab->fixedSize) return false;
            return typesEqual(aa->element, ab->element);
        }

        case ASTKind::RowRefType:
            return typesEqual(a->as<RowRefTypeAST>()->inner,
                              b->as<RowRefTypeAST>()->inner);

        case ASTKind::FunctionType: {
            FunctionTypeAST* fa = a->as<FunctionTypeAST>();
            FunctionTypeAST* fb = b->as<FunctionTypeAST>();

            if (fa->params.size() != fb->params.size()) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                if (!typesEqual(fa->params[i], fb->params[i])) return false;
            }
            return typesEqual(fa->returnType, fb->returnType);
        }

        case ASTKind::UnknownType:
            // Two unknown-type singletons are the same pointer and were
            // caught by the fast path. Reaching here means one side is
            // an unknown and the other is a different kind — not equal.
            return false;

        default:
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// isAssignable
// ─────────────────────────────────────────────────────────────────────────────

bool isAssignable(TypeAST* target, TypeAST* source, SemaContext& ctx) {
    if (!target || !source) return false;

    // ─── Rule 1: identical types ───────────────────────────────────────
    if (typesEqual(target, source)) return true;

    // ─── Rule 2: T → T? (widening to nullable) ─────────────────────────
    //
    // A non-nil value of type `T` can be stored in a location of type `T?`.
    // The reverse — using a `T?` where `T` is expected — is a narrowing
    // and requires an explicit `!= nil` check or `??`; that is enforced at
    // the use site, not here.
    if (isNullableType(target)) {
        TypeAST* innerTarget = unwrapNullable(target);
        return isAssignable(innerTarget, source, ctx);
    }

    // ─── Rule 3: row reference nilability ──────────────────────────────
    //
    // Every `&T` is inherently nilable (§5.2). Assigning a `&T` to a
    // `&T` is identity (rule 1). Assigning `nil` to a `&T` is handled
    // upstream by resolveExprWithTarget's nil-literal case; by the time
    // we see a source here, it has a real type.
    //
    // This branch exists so a `&U` cannot be assigned to a `&T` even
    // when `U` is a table and `T` is a table. That is already covered
    // by `typesEqual` returning false, so there is nothing to add. The
    // comment is here to record that the case is deliberate.

    // ─── Everything else is a mismatch ────────────────────────────────
    //
    // The literal-adaptation rules of §5.8 (an untyped `42` adopting the
    // integer type the context requires) and the `nil` rules of §5.2 are
    // handled by `resolveExprWithTarget` before it calls this function.
    // By the time isAssignable runs, both sides are fully typed and the
    // only valid relationships are identity and widening-to-nullable.
    //
    // No diagnostic is emitted here. The caller (resolveExprWithTarget,
    // resolveReturnStmt, the argument-checking loop in resolveCallExpr)
    // knows the expression, the location, and the context, and emits a
    // message that names what went wrong. isAssignable is a predicate.
    return false;
}

} // namespace lucid::sema