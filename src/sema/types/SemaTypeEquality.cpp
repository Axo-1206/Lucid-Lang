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
        ctx.diagnostics.note(target, "  #floor(x) - round toward -∞");
        ctx.diagnostics.note(target, "  #ceil(x)  - round toward +∞");
        ctx.diagnostics.note(target, "  #round(x) - round to nearest, half away from zero");
        return false;
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