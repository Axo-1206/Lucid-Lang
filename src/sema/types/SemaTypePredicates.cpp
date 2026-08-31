/// @file SemaTypePredicates.cpp
/// @brief Implementation of type predicates.

#include "SemaType.hpp"
#include "core/builtins/BuiltinTypes.hpp"
#include "core/ASTStrings.hpp"
#include "../context/SemaContext.hpp"

#include <unordered_set>

namespace sema {

// ─── Primitive Type Predicates ──────────────────────────────────────────

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

bool isPrimitiveType(TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

// ─── Wrapper Type Predicates ────────────────────────────────────────────

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

bool isBorrowedType(TypeAST* type) {
    if (!type) return false;
    
    // &T is a borrowed type
    if (type->isa<RefTypeAST>()) {
        return true;
    }
    
    // [_]T is a borrowed type (slice)
    if (type->isa<ArrayTypeAST>()) {
        return type->as<ArrayTypeAST>()->isSlice();
    }
    
    return false;
}

// ─── Named Type Predicates ──────────────────────────────────────────────

bool isStructType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* named = const_cast<NamedTypeAST*>(type->as<NamedTypeAST>());
    
    // Ensure resolvedDecl is populated
    resolveNamedType(named, ctx);
    
    return named->resolvedDecl && named->resolvedDecl->isa<StructDeclAST>();
}

bool isEnumType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* named = const_cast<NamedTypeAST*>(type->as<NamedTypeAST>());
    
    // Ensure resolvedDecl is populated
    resolveNamedType(named, ctx);
    
    return named->resolvedDecl && named->resolvedDecl->isa<EnumDeclAST>();
}

bool isTraitType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    NamedTypeAST* named = const_cast<NamedTypeAST*>(type->as<NamedTypeAST>());
    
    // Ensure resolvedDecl is populated
    resolveNamedType(named, ctx);
    
    return named->resolvedDecl && named->resolvedDecl->isa<TraitDeclAST>();
}

bool isGenericParamType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    return ctx.isGenericParam(named->name);
}

// ─── Built-in Type Predicates ────────────────────────────────────────────

bool isArenaType(TypeAST* type) {
    return builtins::isArenaType(const_cast<TypeAST*>(type));
}

bool isArenaDescriptorType(TypeAST* type) {
    return builtins::isArenaDescriptorType(const_cast<TypeAST*>(type));
}

bool isArenaNamedType(NamedTypeAST* named) {
    return builtins::isArenaNamedType(named);
}

bool isArenaDescriptorNamedType(NamedTypeAST* named) {
    return builtins::isArenaDescriptorNamedType(named);
}

bool isArenaBinding(VarDeclAST* decl) {
    if (!decl) return false;
    return isArenaType(decl->type);
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

// ─── Numeric Helpers ─────────────────────────────────────────────────────

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

// ─── Switch Type Checks ──────────────────────────────────────────────────

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
    
    NamedTypeAST* named = const_cast<NamedTypeAST*>(type->as<NamedTypeAST>());
    
    // Ensure resolvedDecl is populated
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
        for (EnumVariantAST* variant : enumDecl->variants) {
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

// ─── FFI Compatibility ──────────────────────────────────────────────────

bool isValidFFIType(const TypeAST* type, SemaContext& ctx) {
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
        NamedTypeAST* named = const_cast<NamedTypeAST*>(type->as<NamedTypeAST>());
        
        // ─── Handle built-in types ──────────────────────────────────────────
        // ArenaDescriptor is a built-in POD type that is FFI-compatible.
        // Arena is NOT FFI-compatible (scope-confined).
        if (isArenaDescriptorNamedType(named)) {
            return true;
        }
        if (isArenaNamedType(named)) {
            return false;  // Arena cannot cross FFI boundary by value
        }
        
        // Ensure resolvedDecl is populated
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

} // namespace sema