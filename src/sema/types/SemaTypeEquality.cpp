/// @file SemaTypeEquality.cpp
/// @brief Implementation of type equality and assignability.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
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
    // 2a. Integer → Float (safe, always allowed)
    if (isFloatType(target) && isIntegerType(source)) {
        return true;
    }
    
    // 2b. Integer → Integer (different sizes, safe promotion only)
    if (isIntegerType(target) && isIntegerType(source)) {
        return isIntegerPromotionSafe(target, source, ctx);
    }
    
    // 2c. Float → Integer (unsafe, reject)
    if (isIntegerType(target) && isFloatType(source)) {
        return false;  // Requires explicit conversion
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

    // ─── 6. Trait conformance ──────────────────────────────────────────────
    if (isTraitType(target, ctx)) {
        NamedTypeAST* namedTarget = target->as<NamedTypeAST>();
        TypeDeclAST* targetDecl = ctx.lookupType(namedTarget->name);
        
        if (targetDecl && targetDecl->isa<TraitDeclAST>()) {
            TraitDeclAST* traitDecl = targetDecl->as<TraitDeclAST>();
            if (isTraitType(source, ctx)) {
                return false;
            }
            return isTraitConformant(source, traitDecl, ctx);
        }
    }

    return false;
}

} // namespace sema