/// @file SemaCompare.cpp
/// @brief Implementation of type comparison and assignability checks.

#include "SemaCompare.hpp"
#include "../context/SemaContext.hpp"
#include "SemaResolve.hpp"
#include "debug/DebugUtils.hpp"

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

            if (fa->hasArrow != fb->hasArrow) return false;

            if (fa->params.size() != fb->params.size()) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                ParamAST* pa = fa->params[i];
                ParamAST* pb = fb->params[i];
                if (pa->isVariadic != pb->isVariadic) return false;
                if (pa->isConst() != pb->isConst()) return false;
                if (!typesEqual(pa->type, pb->type)) return false;
            }

            return typesEqual(fa->returnType, fb->returnType);
        }

        default:
            return false;
    }
}

// ─── Type Unwrapping ─────────────────────────────────────────────────────

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

// ─── Numeric Type Helpers ───────────────────────────────────────────────

size_t getIntegerBitWidth(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return 0;
    
    // Only return a width if this is actually an integer type.
    // Bool and Char are NOT integers in Sema's type system.
    PrimitiveKind kind = type->as<PrimitiveTypeAST>()->primitiveKind;
    
    if (!isIntegerKind(kind)) {
        return 0;
    }
    
    return getPrimitiveBitWidth(kind);
}

TypeAST* getLargerIntegerType(TypeAST* a, TypeAST* b, SemaContext& ctx) {
    if (!a || !b || !isIntegerType(a) || !isIntegerType(b)) return nullptr;
    
    size_t bitsA = getIntegerBitWidth(a);
    size_t bitsB = getIntegerBitWidth(b);
    
    // Return the larger one
    if (bitsA >= bitsB) return a;
    return b;
}

bool isIntegerPromotionSafe(TypeAST* target, TypeAST* source, SemaContext& ctx) {
    if (!target || !source) return false;
    if (!isIntegerType(target) || !isIntegerType(source)) return false;
    
    size_t targetBits = getIntegerBitWidth(target);
    size_t sourceBits = getIntegerBitWidth(source);
    
    // Target must be at least as large as source (no precision loss)
    return targetBits >= sourceBits;
}

// ─── Trait Conformance Helper ──────────────────────────────────────────

static bool isTraitConformant(TypeAST* source, 
                               TraitDeclAST* traitDecl, 
                               SemaContext& ctx) {
    if (!source || !traitDecl) return false;

    if (!source->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* namedSource = source->as<NamedTypeAST>();
    
    // ─── Ensure resolvedDecl is populated ──────────────────────
    // resolveNamedType is a no-op if already resolved, but ensures we have
    // the declaration before checking it.
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

// ─── Type Predicates ─────────────────────────────────────────────────────

bool isNullableType(TypeAST* type) {
    return type && (type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>());
}

bool isFallibleType(TypeAST* type) {
    return type && (type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>());
}

bool isReferenceType(TypeAST* type) {
    return type && type->isa<RefTypeAST>();
}

bool isPointerType(TypeAST* type) {
    return type && type->isa<PtrTypeAST>();
}

bool isPrimitiveType(TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

bool isBoolType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
}

bool isIntegerType(TypeAST* type) {
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
            return false;
    }
}

bool isFloatType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    switch (type->as<PrimitiveTypeAST>()->primitiveKind) {
        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
        case PrimitiveKind::Decimal:
            return true;
        default:
            return false;
    }
}

bool isNumericType(TypeAST* type) {
    return isIntegerType(type) || isFloatType(type);
}

bool isStringType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String;
}

bool isCharType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Char;
}

// ─── Named Type Checks ──────────────────────────────────────────────────

bool isStructType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* named = type->as<NamedTypeAST>();
    
    // ─── Ensure resolvedDecl is populated ──────────────────────
    // resolveNamedType is a no-op if already resolved, but ensures we have
    // the declaration before checking it.
    resolveNamedType(named, ctx);
    
    return named->resolvedDecl && named->resolvedDecl->isa<StructDeclAST>();
}

bool isEnumType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* named = type->as<NamedTypeAST>();
    
    // ─── Ensure resolvedDecl is populated ──────────────────────
    resolveNamedType(named, ctx);
    
    return named->resolvedDecl && named->resolvedDecl->isa<EnumDeclAST>();
}

bool isTraitType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* named = type->as<NamedTypeAST>();
    
    // ─── Ensure resolvedDecl is populated ──────────────────────
    resolveNamedType(named, ctx);
    
    return named->resolvedDecl && named->resolvedDecl->isa<TraitDeclAST>();
}

bool isGenericParamType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    return ctx.isGenericParam(named->name);
}

// ─── Switch Type Checks ─────────────────────────────────────────────────

bool isValidSwitchType(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;

    if (isIntegerType(type)) return true;
    if (isBoolType(type)) return true;
    if (isCharType(type)) return true;
    if (isStringType(type)) return true;
    if (isEnumType(type, ctx)) return true;

    return false;
}

EnumDeclAST* getEnumDeclFromType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return nullptr;
    
    NamedTypeAST* named = type->as<NamedTypeAST>();
    
    // ─── Ensure resolvedDecl is populated ──────────────────────
    resolveNamedType(named, ctx);
    
    if (!named->resolvedDecl || !named->resolvedDecl->isa<EnumDeclAST>()) return nullptr;
    
    return named->resolvedDecl->as<EnumDeclAST>();
}

bool isSwitchCaseCompatible(ExprAST* value, 
                             TypeAST* subjectType, 
                             SemaContext& ctx) {
    if (!value || !subjectType) return false;

    if (isEnumType(subjectType, ctx)) {
        if (!value->isa<FieldAccessExprAST>()) return false;
        const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
        if (!field->object->isa<IdentifierExprAST>()) return false;
        IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
        TypeDeclAST* decl = ctx.lookupType(id->name);
        if (!decl || !decl->isa<EnumDeclAST>()) return false;
        const EnumDeclAST* enumDecl = decl->as<EnumDeclAST>();
        for (const EnumVariantAST* variant : enumDecl->variants) {
            if (variant->name == field->fieldName) return true;
        }
        return false;
    }

    if (isIntegerType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::Int ||
               lit->kind == LiteralKind::Hex ||
               lit->kind == LiteralKind::Binary;
    }

    if (isBoolType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::True ||
               lit->kind == LiteralKind::False;
    }

    if (isCharType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::Char;
    }

    if (isStringType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::String ||
               lit->kind == LiteralKind::RawString;
    }

    return false;
}

// ─── FFI Compatibility ───────────────────────────────────────────────────

bool isValidFFIType(TypeAST* type, SemaContext& ctx) {
    if (!type) return true;

    if (type->isa<PrimitiveTypeAST>()) return true;

    if (type->isa<PtrTypeAST>()) {
        TypeAST* inner = type->as<PtrTypeAST>()->inner;
        
        if (inner->isa<FuncTypeAST>()) {
            FuncTypeAST* funcType = inner->as<FuncTypeAST>();
            for (ParamAST* param : funcType->params) {
                if (!isValidFFIType(param->type, ctx)) return false;
            }
            if (funcType->returnType && !isValidFFIType(funcType->returnType, ctx)) {
                return false;
            }
            return true;
        }
        
        if (inner->isa<ArrayTypeAST>()) return false;
        if (isNullableType(inner) || isFallibleType(inner)) return false;
        if (inner->isa<RefTypeAST>()) return false;
        if (isTraitType(inner, ctx)) return false;
        return isValidFFIType(inner, ctx);
    }

    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        
        // ─── Ensure resolvedDecl is populated ──────────────────────
        resolveNamedType(named, ctx);
        
        TypeDeclAST* decl = named->resolvedDecl;
        if (!decl) return false;
        
        if (decl->isa<TraitDeclAST>()) return false;
        
        if (decl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = decl->as<StructDeclAST>();
            for (FieldDeclAST* field : structDecl->fields) {
                if (!isValidFFIType(field->type, ctx)) return false;
            }
            return true;
        }
        
        if (decl->isa<EnumDeclAST>()) return true;
        return false;
    }

    if (type->isa<ArrayTypeAST>()) {
        return isValidFFIType(type->as<ArrayTypeAST>()->element, ctx);
    }

    if (isNullableType(type) || isFallibleType(type)) return false;
    if (type->isa<FuncTypeAST>()) return false;
    if (type->isa<RefTypeAST>()) return false;

    return false;
}

// ─── BorrowedType ───────────────────────────────────────────────────

bool isBorrowedType(TypeAST* type) {
    if (!type) return false;
    
    // &T is a borrowed type
    if (type->isa<RefTypeAST>()) {
        return true;
    }
    
    // [_]T is a borrowed type (slice)
    if (type->isa<ArrayTypeAST>()) {
        ArrayTypeAST* array = type->as<ArrayTypeAST>();
        if (array->isSlice()) {
            return true;
        }
    }
    
    return false;
}

} // namespace sema