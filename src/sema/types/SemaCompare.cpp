/// @file SemaCompare.cpp
/// @brief Implementation of type comparison and assignability checks.

#include "SemaCompare.hpp"
#include "../context/SemaContext.hpp"
#include "SemaResolve.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// ─── Type Equality ───────────────────────────────────────────────────────

bool typesEqual(const TypeAST* a, const TypeAST* b) {
    if (a == b) return true;   // Same pointer or both null
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case ASTKind::PrimitiveType:
            return a->as<PrimitiveTypeAST>()->primitiveKind
                == b->as<PrimitiveTypeAST>()->primitiveKind;

        case ASTKind::NamedType: {
            const NamedTypeAST* na = a->as<NamedTypeAST>();
            const NamedTypeAST* nb = b->as<NamedTypeAST>();
            if (na->name != nb->name) return false;
            if (na->genericArgs.size() != nb->genericArgs.size()) return false;
            for (size_t i = 0; i < na->genericArgs.size(); ++i) {
                if (!typesEqual(na->genericArgs[i], nb->genericArgs[i])) return false;
            }
            return true;
        }

        // ─── Wrapper Types (T?, T!, T?!) ──────────────────────────────────
        case ASTKind::NullableType:
        case ASTKind::FallibleType:
        case ASTKind::CombinedType:
        case ASTKind::RefType:
        case ASTKind::PtrType: {
            const TypeAST* innerA = nullptr;
            const TypeAST* innerB = nullptr;
            
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

        // ─── Array Type ────────────────────────────────────────────────────
        case ASTKind::ArrayType: {
            const ArrayTypeAST* aa = a->as<ArrayTypeAST>();
            const ArrayTypeAST* ab = b->as<ArrayTypeAST>();
            if (aa->arrayKind != ab->arrayKind) return false;
            if (aa->size != ab->size) return false;
            return typesEqual(aa->element, ab->element);
        }

        // ─── Function Type ────────────────────────────────────────────────
        case ASTKind::FuncType: {
            const FuncTypeAST* fa = a->as<FuncTypeAST>();
            const FuncTypeAST* fb = b->as<FuncTypeAST>();

            if (fa->hasArrow != fb->hasArrow) return false;

            if (fa->params.size() != fb->params.size()) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                const ParamAST* pa = fa->params[i];
                const ParamAST* pb = fb->params[i];
                if (pa->isVariadic != pb->isVariadic) return false;
                if (pa->isConst != pb->isConst) return false;
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

// ─── Assignability ───────────────────────────────────────────────────────

/// @brief Check if a type implements a trait.
static bool isTraitConformant(const TypeAST* source, 
                               const TraitDeclAST* traitDecl, 
                               SemaContext& ctx) {
    if (!source || !traitDecl) return false;

    // Source must be a named type
    if (!source->isa<NamedTypeAST>()) return false;
    
    const NamedTypeAST* namedSource = source->as<NamedTypeAST>();
    
    // Resolve the source type declaration
    const TypeDeclAST* sourceDecl = ctx.lookupType(namedSource->name);
    if (!sourceDecl) return false;

    // Only structs can implement traits
    if (!sourceDecl->isa<StructDeclAST>()) {
        return false;
    }

    const StructDeclAST* structDecl = sourceDecl->as<StructDeclAST>();

    // ─── Direct Check: Look at the struct's traitRefs ──────────────────
    // Since the struct was already validated in resolveStructDecl(),
    // we can just check if the trait is in its traitRefs list.
    for (const NamedTypeAST* traitRef : structDecl->traitRefs) {
        // Resolve the trait reference and compare to the target trait
        const TraitDeclAST* resolvedTrait = resolveTraitRef(traitRef, ctx);
        if (resolvedTrait == traitDecl) {
            return true;
        }
    }
    
    return false;
}

bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx) {
    if (!target || !source) return false;

    // ─── 1. Identical types ──────────────────────────────────────────────
    if (typesEqual(target, source)) return true;

    // ─── 2. T → T? (widening to nullable) ──────────────────────────────
    if (target->isa<NullableTypeAST>()) {
        const TypeAST* inner = target->as<NullableTypeAST>()->inner;
        return isAssignable(inner, source, ctx);
    }

    // ─── 3. T → T! (widening to fallible) ──────────────────────────────
    if (target->isa<FallibleTypeAST>()) {
        const TypeAST* inner = target->as<FallibleTypeAST>()->inner;
        return isAssignable(inner, source, ctx);
    }

    // ─── 4. T → T?! (widening to combined) ─────────────────────────────
    if (target->isa<CombinedTypeAST>()) {
        const TypeAST* inner = target->as<CombinedTypeAST>()->inner;
        
        // T → T?!
        if (isAssignable(inner, source, ctx)) return true;
        
        // T? → T?!
        if (source->isa<NullableTypeAST>() &&
            isAssignable(inner, source->as<NullableTypeAST>()->inner, ctx)) return true;
        
        // T! → T?!
        if (source->isa<FallibleTypeAST>() &&
            isAssignable(inner, source->as<FallibleTypeAST>()->inner, ctx)) return true;
        
        return false;
    }

    // ─── 5. Trait conformance (Trait as target) ──────────────────────────
    if (isTraitType(target, ctx)) {
        const NamedTypeAST* namedTarget = target->as<NamedTypeAST>();
        const TypeDeclAST* targetDecl = ctx.lookupType(namedTarget->name);
        
        if (targetDecl && targetDecl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = targetDecl->as<TraitDeclAST>();
            
            // Traits don't implement other traits
            if (isTraitType(source, ctx)) {
                return false;
            }
            
            // Check if the source struct implements the trait
            return isTraitConformant(source, traitDecl, ctx);
        }
    }

    // ─── 6. Everything else ──────────────────────────────────────────────
    return false;
}

// ─── Type Predicates ─────────────────────────────────────────────────────

// ─── Sentinel Checks ────────────────────────────────────────────────────

bool isNullableType(const TypeAST* type) {
    return type && (type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>());
}

bool isFallibleType(const TypeAST* type) {
    return type && (type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>());
}

// ─── Category Checks ────────────────────────────────────────────────────

bool isReferenceType(const TypeAST* type) {
    return type && type->isa<RefTypeAST>();
}

bool isPointerType(const TypeAST* type) {
    return type && type->isa<PtrTypeAST>();
}

bool isPrimitiveType(const TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

bool isBoolType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
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
            return false;
    }
}

bool isFloatType(const TypeAST* type) {
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

bool isNumericType(const TypeAST* type) {
    return isIntegerType(type) || isFloatType(type);
}

bool isStringType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String;
}

bool isCharType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Char;
}

// ─── Named Type Checks ──────────────────────────────────────────────────

bool isStructType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = ctx.lookupType(named->name);
    return decl && decl->isa<StructDeclAST>();
}

bool isEnumType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = ctx.lookupType(named->name);
    return decl && decl->isa<EnumDeclAST>();
}

bool isTraitType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = ctx.lookupType(named->name);
    return decl && decl->isa<TraitDeclAST>();
}

bool isGenericParamType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    return ctx.isGenericParam(named->name);
}

// ─── Switch Type Checks ──────────────────────────────────────────────────

bool isValidSwitchType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return false;

    if (isIntegerType(type)) return true;
    if (isBoolType(type)) return true;
    if (isCharType(type)) return true;
    if (isStringType(type)) return true;
    if (isEnumType(type, ctx)) return true;

    return false;
}

const EnumDeclAST* getEnumDeclFromType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return nullptr;
    
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = ctx.lookupType(named->name);
    if (!decl || !decl->isa<EnumDeclAST>()) return nullptr;
    
    return decl->as<EnumDeclAST>();
}

bool isSwitchCaseCompatible(const ExprAST* value, 
                             const TypeAST* subjectType, 
                             SemaContext& ctx) {
    if (!value || !subjectType) return false;

    // ─── Enum type: value must be an enum variant ──────────────────────
    if (isEnumType(subjectType, ctx)) {
        if (!value->isa<FieldAccessExprAST>()) return false;
        
        const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
        
        if (!field->object->isa<IdentifierExprAST>()) return false;
        
        const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
        const TypeDeclAST* decl = ctx.lookupType(id->name);
        if (!decl || !decl->isa<EnumDeclAST>()) return false;
        
        const EnumDeclAST* enumDecl = decl->as<EnumDeclAST>();
        for (const EnumVariantAST* variant : enumDecl->variants) {
            if (variant->name == field->fieldName) return true;
        }
        return false;
    }

    // ─── Integer type: value must be an integer literal ────────────────
    if (isIntegerType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::Int ||
               lit->kind == LiteralKind::Hex ||
               lit->kind == LiteralKind::Binary;
    }

    // ─── Bool type: value must be true/false ────────────────────────────
    if (isBoolType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::True ||
               lit->kind == LiteralKind::False;
    }

    // ─── Char type: value must be a char literal ────────────────────────
    if (isCharType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::Char;
    }

    // ─── String type: value must be a string literal ────────────────────
    if (isStringType(subjectType)) {
        if (!value->isa<LiteralExprAST>()) return false;
        const LiteralExprAST* lit = value->as<LiteralExprAST>();
        return lit->kind == LiteralKind::String ||
               lit->kind == LiteralKind::RawString;
    }

    return false;
}

// ─── FFI Compatibility ───────────────────────────────────────────────────

bool isValidFFIType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return true; // void

    // ─── Primitive types ──────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) return true;

    // ─── Raw pointers ─────────────────────────────────────────────────────────
    if (type->isa<PtrTypeAST>()) {
        const TypeAST* inner = type->as<PtrTypeAST>()->inner;
        
        // ─── Function pointers: *() -> T is FFI-compatible ──────────────────
        if (inner->isa<FuncTypeAST>()) {
            // Validate the function type itself (parameters and return types)
            const FuncTypeAST* funcType = inner->as<FuncTypeAST>();
            
            // Check all parameter types
            for (const ParamAST* param : funcType->params) {
                if (!isValidFFIType(param->type, ctx)) {
                    return false;
                }
            }
            
            // Check return type
            if (funcType->returnType && !isValidFFIType(funcType->returnType, ctx)) {
                return false;
            }
            
            return true;  // *() -> T is valid
        }
        
        // ─── Pointers to other types ──────────────────────────────────────────
        // Only allow pointers to: primitives, structs, enums, and other pointers
        if (inner->isa<ArrayTypeAST>()) {
            return false;  // *array is not FFI-compatible
        }
        if (isNullableType(inner) || isFallibleType(inner)) {
            return false;
        }
        if (inner->isa<RefTypeAST>()) {
            return false;
        }
        if (isTraitType(inner, ctx)) {
            return false;
        }
        
        // Recursively validate inner type
        return isValidFFIType(inner, ctx);
    }

    // ─── Named types (structs, enums, traits) ──────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        const TypeDeclAST* decl = ctx.lookupType(named->name);
        if (!decl) return false;

        // Traits are not FFI-compatible
        if (decl->isa<TraitDeclAST>()) return false;

        if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            for (const FieldDeclAST* field : structDecl->fields) {
                if (!isValidFFIType(field->type, ctx)) return false;
            }
            return true;
        }

        if (decl->isa<EnumDeclAST>()) {
            return true; // Enum maps to integer
        }

        return false;
    }

    // ─── Arrays ──────────────────────────────────────────────────────────────
    // Arrays are valid if element type is FFI-compatible
    if (type->isa<ArrayTypeAST>()) {
        return isValidFFIType(type->as<ArrayTypeAST>()->element, ctx);
    }

    // ─── Nullable/fallible types ────────────────────────────────────────────
    if (isNullableType(type) || isFallibleType(type)) {
        return false;
    }

    // ─── Function types (bare, not behind a pointer) ──────────────────────
    // Bare function types are NOT FFI-compatible (must be *func_type)
    if (type->isa<FuncTypeAST>()) {
        return false;
    }

    // ─── Reference types ────────────────────────────────────────────────────
    if (type->isa<RefTypeAST>()) {
        return false;
    }

    return false;
}

} // namespace sema