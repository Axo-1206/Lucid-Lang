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
 *   payload, recursively for any nested type.
 *
 * @note Refactored to use the new SemaContext sub-contexts where needed.
 */

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// typesEqual
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Structural equality of two `TypeAST` nodes — same shape and same
 *        payload, recursively, not the same pointer.
 *
 * How it works: a fast identity check first (`a == b` — also correctly
 * handles both being `nullptr`, which happens for two void-returning
 * `FuncTypeAST`s comparing their empty `returnTypes`), then a `kind`
 * mismatch is an instant `false` — two nodes of different `ASTKind` can
 * never be structurally equal regardless of payload. Past that, it's a
 * `switch` on `a->kind` with one case per `TypeAST` subtype, each
 * comparing exactly the fields that make that subtype what it is:
 *
 *   - `PrimitiveType` — the two `PrimitiveKind` enum values must match.
 *   - `NamedType` — the two `name`s must match (see this file's top-level
 *     "Structural equality over TypeAST" note for why name equality is
 *     what's used, not resolved-declaration identity), AND `genericArgs`
 *     must be the same length with each pair recursively `typesEqual()` —
 *     so `Buffer<int>` and `Buffer<string>` are correctly unequal.
 *   - `NullableType` / `FallibleType` / `CombinedType` / `RefType` /
 *     `PtrType` — all five are "one wrapper, one inner type" shapes, so
 *     each case just recurses into `->inner`. (Note this only fires when
 *     both sides share the *same* wrapper kind — a `NullableType` is never
 *     `typesEqual()` to a `CombinedType` even if their inner types match,
 *     since `T?` and `T?!` are different types with different state
 *     counts — see CombinedTypeAST's own doc comment in TypeAST.hpp.)
 *   - `ArrayType` — `arrayKind` (Slice/Dynamic/Fixed) must match, `size`
 *     must match (harmless to compare unconditionally for Slice/Dynamic,
 *     which always carry `0`), and `element` must recursively match.
 *   - `FuncType` — same parameter count with each pair's `isVariadic`,
 *     `isConst`, and `type` all matching (parameter *names* are not
 *     compared — a type is about shape, not spelling), then the same
 *     check repeated for `returnTypes`.
 *   - Anything else (a `TypeAST` subkind not listed above) — treated as
 *     "can't prove equal" rather than hit with an assert, since a new
 *     `TypeAST` subtype can be added to TypeAST.hpp without every caller
 *     of `typesEqual()` being audited first.
 *
 * @param a One of the two types to compare (either may be `nullptr`).
 * @param b The other type to compare.
 * @return `true` if `a` and `b` describe the same type structurally.
 */
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

/**
 * @brief True if `type` carries the `nil` sentinel — either `T?` on its
 *        own, or the three-state `T?!` combination.
 *
 * How it works: a plain `ASTKind` check, `NullableTypeAST` or
 * `CombinedTypeAST` — no recursion, since nullability is a single outer
 * wrapper by construction (the parser never produces `T??`, and `T?!` is
 * its own distinct node rather than a `NullableTypeAST` wrapping a
 * `FallibleTypeAST` — see CombinedTypeAST's doc comment in TypeAST.hpp).
 * `FallibleTypeAST` alone (`T!`) is deliberately excluded — it carries
 * `err`, not `nil`.
 *
 * @param type The type to check (may be `nullptr` — returns `false`).
 */
bool isNullableType(const TypeAST* type) {
    return type && (type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>());
}

/**
 * @brief True if `type` carries the `err` sentinel — either `T!` on its
 *        own, or the three-state `T?!` combination.
 *
 * How it works: the mirror image of `isNullableType()` above — same
 * reasoning, just checking `FallibleTypeAST`/`CombinedTypeAST` instead of
 * `NullableTypeAST`/`CombinedTypeAST`.
 *
 * @param type The type to check (may be `nullptr` — returns `false`).
 */
bool isFallibleType(const TypeAST* type) {
    return type && (type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>());
}

/**
 * @brief Strip one layer of `?` (or the nullable half of `?!`), returning
 *        the plain inner type.
 *
 * How it works: three cases, checked in order. `NullableTypeAST` and
 * `CombinedTypeAST` both unwrap to their `->inner` (for `T?!`, this
 * deliberately also strips the fallible half in the same step — there is
 * no intermediate "`T!` with nullability removed" node to return, since
 * `T?!` isn't `NullableTypeAST` wrapping `FallibleTypeAST` in the first
 * place). Anything else — including a type that was never nullable to
 * begin with — is returned unchanged; this makes the function safe to
 * call unconditionally rather than needing an `isNullableType()` guard at
 * every call site.
 *
 * @param type The type to unwrap (may be `nullptr` — returned unchanged).
 * @return The inner type, or `type` itself if it carried no `?`/`?!`.
 */
TypeAST* unwrapNullable(TypeAST* type) {
    if (!type) return type;
    if (type->isa<NullableTypeAST>()) return type->as<NullableTypeAST>()->inner;
    if (type->isa<CombinedTypeAST>()) return type->as<CombinedTypeAST>()->inner;
    return type;
}

/**
 * @brief Strip one layer of `!` (or the fallible half of `?!`), returning
 *        the plain inner type.
 *
 * How it works: the mirror image of `unwrapNullable()` above — same
 * three-case shape and the same "safe to call unconditionally" guarantee,
 * just unwrapping `FallibleTypeAST`/`CombinedTypeAST` instead.
 *
 * @param type The type to unwrap (may be `nullptr` — returned unchanged).
 * @return The inner type, or `type` itself if it carried no `!`/`?!`.
 */
TypeAST* unwrapFallible(TypeAST* type) {
    if (!type) return type;
    if (type->isa<FallibleTypeAST>()) return type->as<FallibleTypeAST>()->inner;
    if (type->isa<CombinedTypeAST>()) return type->as<CombinedTypeAST>()->inner;
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// Numeric / Integer predicates
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief True if `type` is any primitive number — integer or
 *        floating-point.
 *
 * How it works: first confirms `type` is a `PrimitiveTypeAST` at all (a
 * `NamedTypeAST`, `ArrayTypeAST`, etc. is never numeric, no matter what it
 * names), then switches on `PrimitiveKind` and returns `true` for every
 * signed/unsigned integer kind (both the machine-dependent aliases —
 * `Byte`..`Ulong` — and the fixed-width ones — `Int8`..`Uint64`, see
 * PrimitiveKind's own doc comment in TypeAST.hpp for why both sets exist)
 * plus the three floating-point kinds (`Float`, `Double`, `Decimal`).
 * `Bool`, `String`, and `Char` fall through to the `default: false` case —
 * deliberately not listed one-by-one, since new non-numeric primitives
 * shouldn't need a matching entry added here to stay excluded.
 *
 * @param type The type to check (may be `nullptr` — returns `false`).
 */
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

/**
 * @brief True if `type` is a primitive *integer* specifically — a
 *        stricter check than `isNumericType()`.
 *
 * How it works: same shape as `isNumericType()` above (confirm
 * `PrimitiveTypeAST` first, then switch on `PrimitiveKind`), but the
 * `case` list stops after the sixteen integer kinds — `Float`, `Double`,
 * and `Decimal` are not listed, so they fall through to `default: false`
 * along with `Bool`/`String`/`Char`. Every kind this returns `true` for is
 * also one `isNumericType()` returns `true` for; the reverse isn't true
 * for the three floating-point kinds, which is the whole reason both
 * functions exist separately (e.g. for rejecting `2.5` where an array
 * index or bitwise-operand position requires a whole number).
 *
 * @param type The type to check (may be `nullptr` — returns `false`).
 */
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

/**
 * @brief Can a value of type `source` be used where `target` is required —
 *        e.g. a `let` initializer, a call argument, a return value?
 *
 * How it works: builds outward from exact equality, adding only the
 * specific widenings Lucid's grammar documents, in this order:
 *
 *   1. **Identical types** (`typesEqual(target, source)`) are always
 *      assignable — the base case everything else falls back on.
 *   2. **Target is `T?`, `T!`, or `T?!`, source is plain `T`** — handled
 *      by recursing into `target`'s `->inner` and comparing against
 *      `source` unchanged. This is a strict one-way widening: going the
 *      other way (`T?` used where `T` is required) is NOT assignability,
 *      it's narrowing — a control-flow-sensitive question ("has this
 *      branch already ruled out `nil`?") that a purely structural
 *      function like this one has no branch information to answer; that
 *      check happens at the use site instead (inside whichever `checkExpr`
 *      rule sees the narrowed branch), not here.
 *   3. **Target is `T?!` specifically** gets one more case beyond step 2:
 *      since `T?!` is the union of `nil`, `err`, and `T`, its own `->inner`
 *      is checked against `source` three additional ways — `source` being
 *      `T?` alone, `T!` alone, or `T?!` itself — each by recursing with
 *      *their* inner type against `target`'s inner. So `let x int?! = y`
 *      accepts a plain `int`, an `int?`, an `int!`, or another `int?!` for
 *      `y`, but not (say) a `string?`.
 *   4. **Everything else is a mismatch.** Notably: arrays, references, and
 *      pointers are NOT covariant here — `[*]Cat` is never assignable
 *      where `[*]Animal` is required, even if such a relationship existed
 *      elsewhere in the type system, because nothing in Grammar.md or
 *      TypeAST.hpp documents array/ref/pointer covariance as a rule this
 *      function should implement.
 *
 * @param target The required type (the declared type of a `let`, a
 *               parameter's type, a function's return type, ...).
 * @param source The type actually being offered.
 * @param ctx    Reserved for a not-yet-implemented trait-conformance case
 *               (see the `// X` note in the body) — unused otherwise.
 * @return `true` if a `source` value can be used directly where `target`
 *         is required, under the widening rules above.
 */
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
    // TypeDeclAST (via resolveTypeNameOrError()/ctx.symbols.lookupType()) and
    // walking `traitRefs`, plus knowing how validateTraitImplementation()
    // exposes its result for reuse here — neither is settled by the files
    // this was written against, so it's intentionally unhandled rather
    // than guessed at.
    (void)ctx;  // reserved for the trait-conformance case above

    // Arrays, references, and pointers are invariant in Lucid — nothing in
    // Grammar.md or TypeAST.hpp documents a covariant case for `[*]T`,
    // `&T`, or `*T`, so anything past this point is a genuine mismatch.
    return false;
}

} // namespace sema