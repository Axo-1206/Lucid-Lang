/// @file SemaTypeEquality.cpp
/// @brief Implementation of type equality and assignability.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/TypeAST.hpp"

namespace sema {

// ─── Type Equality ───────────────────────────────────────────────────────

bool typesEqual(TypeAST* a, TypeAST* b) {
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

            // ─── Fast path: both resolved ────────────────────────────────────
            //
            // After the storage layer, two `Box<int>` at different call sites
            // share the same `resolvedDecl` pointer — both were resolved against
            // `SemaContext::genericTypeInstantiations` and bound to the same
            // canonical StructDeclAST*. Comparing pointers is O(1) and does not
            // depend on whether the two nodes' `genericArgs` were canonicalized
            // identically.
            //
            // The name check is kept because two nodes with different names can
            // still both have resolvedDecl set — e.g. an unresolved `Foo` and a
            // resolved `Bar` — and `name` is what the reader actually wrote.
            // Pointer equality on resolvedDecl implies name equality for the
            // struct case (both resolved through the same storage key), but
            // not for enums or traits, which don't go through the storage map.
            if (na->resolvedDecl && nb->resolvedDecl) {
                if (na->resolvedDecl != nb->resolvedDecl) return false;
                // Same decl → same type, regardless of how the args were written.
                return true;
            }

            // ─── Slow path: at least one side unresolved ─────────────────────
            //
            // Fall back to structural comparison. This covers the case of a
            // NamedTypeAST that hasn't been through resolution yet (e.g. one
            // side is a freshly-parsed type annotation being checked against
            // an already-resolved value type).
            if (na->name != nb->name) return false;
            if (na->genericArgs.size() != nb->genericArgs.size()) return false;
            for (size_t i = 0; i < na->genericArgs.size(); ++i) {
                if (!typesEqual(na->genericArgs[i], nb->genericArgs[i])) return false;
            }
            return true;
        }

        case ASTKind::NullableType:
        case ASTKind::FallibleType:
        case ASTKind::CombinedType:
        case ASTKind::RefType:
        case ASTKind::PtrType: {
            TypeAST* innerA = nullptr;
            TypeAST* innerB = nullptr;
            
            if (a->isa<NullableTypeAST>()) {
                innerA = a->as<NullableTypeAST>()->inner;
                innerB = b->as<NullableTypeAST>()->inner;
            } else if (a->isa<FallibleTypeAST>()) {
                innerA = a->as<FallibleTypeAST>()->inner;
                innerB = b->as<FallibleTypeAST>()->inner;
            } else if (a->isa<CombinedTypeAST>()) {
                innerA = a->as<CombinedTypeAST>()->inner;
                innerB = b->as<CombinedTypeAST>()->inner;
            } else if (a->isa<RefTypeAST>()) {
                innerA = a->as<RefTypeAST>()->inner;
                innerB = b->as<RefTypeAST>()->inner;
            } else if (a->isa<PtrTypeAST>()) {
                innerA = a->as<PtrTypeAST>()->inner;
                innerB = b->as<PtrTypeAST>()->inner;
            }
            return typesEqual(innerA, innerB);
        }

        case ASTKind::ArrayType: {
            ArrayTypeAST* aa = a->as<ArrayTypeAST>();
            ArrayTypeAST* ab = b->as<ArrayTypeAST>();
            if (aa->arrayKind != ab->arrayKind) return false;
            if (aa->size != ab->size) return false;
            return typesEqual(aa->element, ab->element);
        }

        case ASTKind::FuncType: {
            FuncTypeAST* fa = a->as<FuncTypeAST>();
            FuncTypeAST* fb = b->as<FuncTypeAST>();

            // ─── Shape must match exactly ──────────────────────────────────────
            //
            // `fn (int) -> int` and `cls (int) -> int` are distinct types: one
            // is a bare function pointer, the other a {func, env} fat pointer
            // with a refcounted environment. They are not interchangeable at
            // the type level — the implicit `fn → cls` coercion is a *value*
            // conversion applied at assignability sites, not an identity.
            //
            // The shape is per-stage, and each nested FuncTypeAST carries its
            // own, so this check runs once per stage as `typesEqual` recurses
            // through `returnType`. That's exactly what's wanted: a mismatch
            // in any stage of a curry chain makes the whole chain unequal.
            if (fa->shape != fb->shape) return false;

            if (fa->params.size() != fb->params.size()) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                ParamAST* pa = fa->params[i];
                ParamAST* pb = fb->params[i];
                if (pa->isVariadic != pb->isVariadic) return false;
                if (pa->isConstParam != pb->isConstParam) return false;
                if (!typesEqual(pa->type, pb->type)) return false;
            }

            return typesEqual(fa->returnType, fb->returnType);
        }

        default:
            return false;
    }
}

// ─── Trait Conformance Helper ──────────────────────────────────────────

static bool isTraitConformant(TypeAST* source, 
                               TraitDeclAST* traitDecl, 
                               SemaContext& ctx) {
    if (!source || !traitDecl) return false;

    if (!source->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* namedSource = source->as<NamedTypeAST>();
    
    // Ensure resolvedDecl is populated
    resolveNamedType(namedSource, ctx);
    
    TypeDeclAST* sourceDecl = namedSource->resolvedDecl;
    if (!sourceDecl) return false;

    if (!sourceDecl->isa<StructDeclAST>()) return false;

    StructDeclAST* structDecl = sourceDecl->as<StructDeclAST>();

    for (NamedTypeAST* traitRef : structDecl->traitRefs) {
        TraitDeclAST* resolvedTrait = resolveTraitRef(traitRef, ctx);
        if (resolvedTrait == traitDecl) {
            return true;
        }
    }
    return false;
}

// ─── Assignability ───────────────────────────────────────────────────────

bool isAssignable(TypeAST* target, TypeAST* source, SemaContext& ctx) {
    if (!target || !source) return false;

    // ─── 1. Identical types ──────────────────────────────────────────────
    if (typesEqual(target, source)) return true;

    // ─── 2. Numeric conversions ──────────────────────────────────────────
    
    // 2a. Integer → Float (safe, always allowed - IMPLICIT WIDENING)
    if (isFloatType(target) && isIntegerType(source)) {
        return true;
    }
    
    // 2b. Integer → Integer (different sizes, safe promotion only)
    if (isIntegerType(target) && isIntegerType(source)) {
        return isIntegerPromotionSafe(target, source, ctx);
    }
    
    // 2c. Float → Integer (unsafe, REJECT - EXPLICIT NARROWING REQUIRED)
    if (isIntegerType(target) && isFloatType(source)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, target,
                              "cannot implicitly convert float to int");
        ctx.diagnostics.note(target, "Use one of these explicit conversion intrinsics:");
        ctx.diagnostics.note(target, "  #trunc(x) - truncate toward zero (C-style)");
        ctx.diagnostics.note(target, "  #floor(x) - round toward negative infinity");
        ctx.diagnostics.note(target, "  #ceil(x)  - round toward positive infinity");
        ctx.diagnostics.note(target, "  #round(x) - round to nearest, half away from zero");
        return false;
    }

    // ─── 2d. fn (T) -> U  →  cls (T) -> U  (implicit widening) ─────────────
    //
    // A bare function pointer can be used wherever a capturing closure is
    // expected, by wrapping it in a null-environment fat pointer at the
    // assignment site. The environment is empty, so no retain/release traffic
    // is generated for the wrapper — it's a zero-cost construction.
    //
    // The reverse is rejected: a `cls` value might have captured variables,
    // and there is no way to strip the environment and produce a valid bare
    // function pointer that preserves the closure's behavior. The user must
    // change the slot's declared shape to `cls`, or refactor the closure to
    // not capture.
    //
    // The check is per-stage: the coercion applies to the *outermost* stage,
    // and inner stages are compared structurally. This matches the design's
    // per-stage marker rule — a `fn (a int) cls (b int) -> int` value can be
    // used where `cls (a int) cls (b int) -> int` is expected (outer stage
    // widens; inner stage already matches), but not where
    // `cls (a int) fn (b int) -> int` is expected (inner stage would need
    // the forbidden `cls → fn` direction).
    if (target->isa<FuncTypeAST>() && source->isa<FuncTypeAST>()) {
        FuncTypeAST* targetFunc = target->as<FuncTypeAST>();
        FuncTypeAST* sourceFunc = source->as<FuncTypeAST>();

        // Only `fn → cls` is permitted. `cls → fn` is rejected below,
        // and equal shapes are already handled by `typesEqual` above.
        if (sourceFunc->shape == FuncShape::Fn &&
            targetFunc->shape == FuncShape::Cls) {
            // Outer stage widens. Now check the rest of the signature
            // structurally: same params, same return type, same inner-stage
            // shapes. Reuse `typesEqual` on the *parts* rather than the
            // whole type, so the shape check on this stage is bypassed
            // (that's what the coercion is).
            if (targetFunc->params.size() != sourceFunc->params.size()) return false;
            for (size_t i = 0; i < targetFunc->params.size(); ++i) {
                ParamAST* tp = targetFunc->params[i];
                ParamAST* sp = sourceFunc->params[i];
                if (tp->isVariadic != sp->isVariadic) return false;
                if (tp->isConstParam != sp->isConstParam) return false;
                if (!typesEqual(tp->type, sp->type)) return false;
            }
            return typesEqual(targetFunc->returnType, sourceFunc->returnType);
        }

        // `cls → fn` is explicitly rejected with a targeted diagnostic,
        // because "why doesn't this work?" is a common question and the
        // generic "types don't match" message doesn't explain it.
        if (sourceFunc->shape == FuncShape::Cls &&
            targetFunc->shape == FuncShape::Fn) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, source,
                                "cannot use a closure ('cls') where a bare function "
                                "pointer ('fn') is expected");
            ctx.diagnostics.note(source,
                                "A 'cls' value may have captured variables, so it "
                                "cannot be unwrapped to a bare function pointer");
            ctx.diagnostics.note(target,
                                "Change the target's shape to 'cls', or refactor the "
                                "closure to not capture");
            return false;
        }

        // Both same shape but structurally different (params/return mismatch):
        // fall through to the generic failure at the bottom of the function,
        // which reports the structural mismatch.
    }

    // ─── 3. T → T? (widening to nullable) ──────────────────────────────
    if (target->isa<NullableTypeAST>()) {
        TypeAST* inner = target->as<NullableTypeAST>()->inner;
        return isAssignable(inner, source, ctx);
    }

    // ─── 4. T → T! (widening to fallible) ──────────────────────────────
    if (target->isa<FallibleTypeAST>()) {
        TypeAST* inner = target->as<FallibleTypeAST>()->inner;
        return isAssignable(inner, source, ctx);
    }

    // ─── 5. T → T?! (widening to combined) ─────────────────────────────
    if (target->isa<CombinedTypeAST>()) {
        TypeAST* inner = target->as<CombinedTypeAST>()->inner;
        if (isAssignable(inner, source, ctx)) return true;
        if (source->isa<NullableTypeAST>() &&
            isAssignable(inner, source->as<NullableTypeAST>()->inner, ctx)) return true;
        if (source->isa<FallibleTypeAST>() &&
            isAssignable(inner, source->as<FallibleTypeAST>()->inner, ctx)) return true;
        return false;
    }

    // ─── 6. T? → T (narrowing from nullable) ────────────────────────────
    // A nullable value cannot be used as plain T without narrowing
    if (source->isa<NullableTypeAST>()) {
        TypeAST* sourceInner = source->as<NullableTypeAST>()->inner;
        if (typesEqual(target, sourceInner)) {
            ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, target,
                                  "cannot use nullable value '", 
                                  typeToString(source, ctx.pool), 
                                  "' as plain '", 
                                  typeToString(target, ctx.pool), 
                                  "'");
            ctx.diagnostics.note(target,
                                 "Narrow the value first using 'if x != nil' or 'x ?? default'");
            return false;
        }
        // Try to assign the inner type to the target (e.g., T? → U where U != T)
        return isAssignable(target, sourceInner, ctx);
    }

    // ─── 7. T! → T (narrowing from fallible) ────────────────────────────
    if (source->isa<FallibleTypeAST>()) {
        TypeAST* sourceInner = source->as<FallibleTypeAST>()->inner;
        if (typesEqual(target, sourceInner)) {
            ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, target,
                                  "cannot use fallible value '", 
                                  typeToString(source, ctx.pool), 
                                  "' as plain '", 
                                  typeToString(target, ctx.pool), 
                                  "'");
            ctx.diagnostics.note(target,
                                 "Narrow the value first using 'if x != err' or 'x ?? default'");
            return false;
        }
        return isAssignable(target, sourceInner, ctx);
    }

    // ─── 8. T?! → T (narrowing from combined) ────────────────────────────
    if (source->isa<CombinedTypeAST>()) {
        TypeAST* sourceInner = source->as<CombinedTypeAST>()->inner;
        if (typesEqual(target, sourceInner)) {
            ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, target,
                                  "cannot use combined value '", 
                                  typeToString(source, ctx.pool), 
                                  "' as plain '", 
                                  typeToString(target, ctx.pool), 
                                  "'");
            ctx.diagnostics.note(target,
                                 "Narrow the value first using 'if x != nil and x != err'");
            return false;
        }
        return isAssignable(target, sourceInner, ctx);
    }

    // ─── 9. Trait conformance ─────────────────────────────────────────────
    if (target->isa<NamedTypeAST>()) {
        NamedTypeAST* namedTarget = target->as<NamedTypeAST>();
        TypeDeclAST* targetDecl = namedTarget->resolvedDecl;
        if (targetDecl && targetDecl->isa<TraitDeclAST>()) {
            TraitDeclAST* traitDecl = targetDecl->as<TraitDeclAST>();
            return isTraitConformant(source, traitDecl, ctx);
        }
    }

    return false;
}

} // namespace sema