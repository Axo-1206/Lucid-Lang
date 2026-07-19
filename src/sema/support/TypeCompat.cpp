/**
 * @file TypeCompat.cpp
 * @brief Implements Sema.hpp's "Type Compatibility Helpers" — typesEqual(),
 *        isAssignable(), isNullableType()/isFallibleType(),
 *        unwrapNullable()/unwrapFallible(), isNumericType()/isIntegerType().
 *
 * @architectural_note Structural equality over TypeAST, not a resolved Type
 *   TypeAST.hpp's file-level note says these nodes represent types "as
 *   written in source" and that "the semantic pass later resolves these
 *   into actual resolved Type objects" — but every function Sema.hpp
 *   actually declares here (typesEqual, isAssignable, ...) takes
 *   `const TypeAST*`, not some separate resolved-Type type. Whatever that
 *   later resolved representation turns out to be, it isn't part of the
 *   files this was written against, so this file compares TypeAST nodes
 *   directly and structurally: same ASTKind, then same kind-specific
 *   payload, recursively for any nested type. This is exactly what
 *   `NamedTypeAST`'s own comparison relies on — see typesEqual()'s
 *   `NamedType` case below for the resulting limitation.
 *
 */

#include "../Sema.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// typesEqual
// ─────────────────────────────────────────────────────────────────────────────

bool typesEqual(const TypeAST* a, const TypeAST* b) {
    if (a == b) {
        return true;   // covers both-null (e.g. two void-return FuncTypeASTs)
    }
    if (!a || !b) {
        return false;
    }
    if (a->kind != b->kind) {
        return false;
    }

    switch (a->kind) {
        case ASTKind::PrimitiveType:
            return a->as<PrimitiveTypeAST>()->primitiveKind
                == b->as<PrimitiveTypeAST>()->primitiveKind;

        case ASTKind::NamedType: {
            const NamedTypeAST* na = a->as<NamedTypeAST>();
            const NamedTypeAST* nb = b->as<NamedTypeAST>();
            // Name equality, not resolved-declaration identity — see this
            // file's "Structural equality over TypeAST" note above.
            // Correct as long as the same name always resolves to the
            // same declaration within one already-name-resolved program;
            // it would NOT distinguish two different modules' unrelated
            // types that happen to share a short name, but nothing in
            // this codebase gives typesEqual() a resolved declaration to
            // compare instead.
            if (na->name != nb->name) return false;
            if (na->genericArgs.size() != nb->genericArgs.size()) return false;
            for (size_t i = 0; i < na->genericArgs.size(); ++i) {
                if (!typesEqual(na->genericArgs[i], nb->genericArgs[i])) return false;
            }
            return true;
        }

        case ASTKind::NullableType:
            return typesEqual(a->as<NullableTypeAST>()->inner, b->as<NullableTypeAST>()->inner);

        case ASTKind::FallibleType:
            return typesEqual(a->as<FallibleTypeAST>()->inner, b->as<FallibleTypeAST>()->inner);

        case ASTKind::CombinedType:
            return typesEqual(a->as<CombinedTypeAST>()->inner, b->as<CombinedTypeAST>()->inner);

        case ASTKind::RefType:
            return typesEqual(a->as<RefTypeAST>()->inner, b->as<RefTypeAST>()->inner);

        case ASTKind::PtrType:
            return typesEqual(a->as<PtrTypeAST>()->inner, b->as<PtrTypeAST>()->inner);

        case ASTKind::ArrayType: {
            const ArrayTypeAST* aa = a->as<ArrayTypeAST>();
            const ArrayTypeAST* ab = b->as<ArrayTypeAST>();
            if (aa->arrayKind != ab->arrayKind) return false;
            // `size` is only meaningful for Fixed (see ArrayTypeAST's own
            // doc comment — "ignored otherwise, should be 0"), so comparing
            // it unconditionally is safe: Slice/Dynamic always carry 0.
            if (aa->size != ab->size) return false;
            return typesEqual(aa->element, ab->element);
        }

        case ASTKind::FuncType: {
            const FuncTypeAST* fa = a->as<FuncTypeAST>();
            const FuncTypeAST* fb = b->as<FuncTypeAST>();

            if (fa->params.size() != fb->params.size()) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                const ParamAST* pa = fa->params[i];
                const ParamAST* pb = fb->params[i];
                if (pa->isVariadic != pb->isVariadic) return false;
                if (pa->isConst != pb->isConst) return false;
                if (!typesEqual(pa->type, pb->type)) return false;
            }

            if (fa->returnTypes.size() != fb->returnTypes.size()) return false;
            for (size_t i = 0; i < fa->returnTypes.size(); ++i) {
                if (!typesEqual(fa->returnTypes[i], fb->returnTypes[i])) return false;
            }
            return true;
        }

        default:
            // Not a TypeAST subkind this switch knows about — treat as
            // "can't prove equal" rather than asserting, since new type
            // kinds may be added to TypeAST.hpp without every caller of
            // typesEqual() being audited first.
            return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Nullable / Fallible predicates and unwrapping
// ─────────────────────────────────────────────────────────────────────────────

bool isNullableType(const TypeAST* type) {
    return type && (type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>());
}

bool isFallibleType(const TypeAST* type) {
    return type && (type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>());
}

TypeAST* unwrapNullable(TypeAST* type) {
    if (!type) return type;
    if (type->isa<NullableTypeAST>()) return type->as<NullableTypeAST>()->inner;
    if (type->isa<CombinedTypeAST>()) return type->as<CombinedTypeAST>()->inner;
    return type;
}

TypeAST* unwrapFallible(TypeAST* type) {
    if (!type) return type;
    if (type->isa<FallibleTypeAST>()) return type->as<FallibleTypeAST>()->inner;
    if (type->isa<CombinedTypeAST>()) return type->as<CombinedTypeAST>()->inner;
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// Numeric / Integer predicates
// ─────────────────────────────────────────────────────────────────────────────

bool isNumericType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    switch (type->as<PrimitiveTypeAST>()->primitiveKind) {
        case PrimitiveKind::Byte:
        case PrimitiveKind::Short:
        case PrimitiveKind::Int:
        case PrimitiveKind::Long:
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Ushort:
        case PrimitiveKind::Uint:
        case PrimitiveKind::Ulong:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
        case PrimitiveKind::Decimal:
            return true;
        default:
            return false;   // Bool, String, Char
    }
}

bool isIntegerType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    switch (type->as<PrimitiveTypeAST>()->primitiveKind) {
        case PrimitiveKind::Byte:
        case PrimitiveKind::Short:
        case PrimitiveKind::Int:
        case PrimitiveKind::Long:
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Ushort:
        case PrimitiveKind::Uint:
        case PrimitiveKind::Ulong:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
            return true;
        default:
            return false;   // Float, Double, Decimal, Bool, String, Char
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// isAssignable
// ─────────────────────────────────────────────────────────────────────────────

bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx) {
    if (!target || !source) {
        return false;
    }

    if (typesEqual(target, source)) {
        return true;
    }

    // T is assignable to T?, T!, and T?! — widening into a sentinel-
    // carrying type never loses information (see NullableTypeAST/
    // FallibleTypeAST's own "Grammar rules enforced by the semantic pass"
    // notes in TypeAST.hpp). The reverse (T? -> T) requires narrowing,
    // which is control-flow-sensitive (see Grammar.md's "Type narrowing"
    // section) and NOT something this purely structural function can
    // decide — that's checked at the use site inside a narrowed branch,
    // not here.
    if (target->isa<NullableTypeAST>()) {
        return isAssignable(target->as<NullableTypeAST>()->inner, source, ctx);
    }
    if (target->isa<FallibleTypeAST>()) {
        return isAssignable(target->as<FallibleTypeAST>()->inner, source, ctx);
    }
    if (target->isa<CombinedTypeAST>()) {
        // T?! accepts plain T, T? alone, T! alone, or T?! itself — any
        // source whose "definite" type is assignable to T?!'s own inner T.
        const TypeAST* inner = target->as<CombinedTypeAST>()->inner;
        if (isAssignable(inner, source, ctx)) return true;
        if (source->isa<NullableTypeAST>() &&
            isAssignable(inner, source->as<NullableTypeAST>()->inner, ctx)) return true;
        if (source->isa<FallibleTypeAST>() &&
            isAssignable(inner, source->as<FallibleTypeAST>()->inner, ctx)) return true;
        if (source->isa<CombinedTypeAST>() &&
            isAssignable(inner, source->as<CombinedTypeAST>()->inner, ctx)) return true;
        return false;
    }

    // Trait-typed target (`let x Named = someStruct` — TraitDeclAST is a
    // TypeDeclAST, so a trait name can appear as a NamedTypeAST target):
    // assignable if `source`'s struct declaration lists that trait in its
    // own `traitRefs` (see StructDeclAST's doc comment in DeclAST.hpp).
    // Deciding that requires resolving `target`/`source` down to their
    // TypeDeclAST (via resolveTypeNameOrError()/ctx.lookupType()) and
    // walking `traitRefs`, plus knowing how validateTraitImplementation()
    // exposes its result for reuse here — neither is settled by the files
    // this was written against, so it's intentionally unhandled rather
    // than guessed at.                                                // X
    (void)ctx;  // reserved for the trait-conformance case above

    // Arrays, references, and pointers are invariant in Lucid — nothing in
    // Grammar.md or TypeAST.hpp documents a covariant case for `[*]T`,
    // `&T`, or `*T`, so anything past this point is a genuine mismatch.
    return false;
}