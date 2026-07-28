/// @file SemaType.cpp
/// @brief Implementation of type resolution.

#include "SemaType.hpp"
#include "SemaLookup.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// =============================================================================
// resolveType - Main Entry Point
// =============================================================================

TypeAST* resolveType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType: return resolvePrimitiveType(type->as<PrimitiveTypeAST>(), ctx);
        case ASTKind::NamedType:     return resolveNamedType(type->as<NamedTypeAST>(), ctx);
        case ASTKind::ArrayType:     return resolveArrayType(type->as<ArrayTypeAST>(), ctx);
        case ASTKind::NullableType:  return resolveNullableType(type->as<NullableTypeAST>(), ctx);
        case ASTKind::FallibleType:  return resolveFallibleType(type->as<FallibleTypeAST>(), ctx);
        case ASTKind::CombinedType:  return resolveCombinedType(type->as<CombinedTypeAST>(), ctx);
        case ASTKind::RefType:       return resolveRefType(type->as<RefTypeAST>(), ctx);
        case ASTKind::PtrType:       return resolvePtrType(type->as<PtrTypeAST>(), ctx);
        case ASTKind::FuncType:      return resolveFuncType(type->as<FuncTypeAST>(), ctx);
        default:
            ctx.error(type, DiagCode::E3003, "unknown type");
            return nullptr;
    }
}

// =============================================================================
// resolvePrimitiveType - Built-in types
// =============================================================================

TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx) {
    // Primitive types are always valid - they're built-in
    // Just return the type as-is
    return const_cast<PrimitiveTypeAST*>(type);
}

// =============================================================================
// resolveNamedType - User-defined types and generic params
// =============================================================================

TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── 1. Check: Is this a generic parameter? ──────────────────────────
    // Generic parameters have highest priority and shadow type names
    if (isGenericParam(type->name, ctx)) {
        // This is a generic parameter - it's valid but not a concrete type
        // Return the named type as-is (it will be resolved by the generic instantiator)
        return const_cast<NamedTypeAST*>(type);
    }

    // ─── 2. Look up as concrete type ──────────────────────────────────────
    const TypeDeclAST* decl = lookupType(type->name, ctx);
    if (!decl) {
        ctx.error(type, DiagCode::E2002,
                  "undefined type '", ctx.pool().lookup(type->name), "'");
        return nullptr;
    }

    // ─── 3. Resolve generic arguments if present ─────────────────────────
    if (!type->genericArgs.empty()) {
        // TODO: Check generic argument arity against the declaration's generic parameters
        // TODO: Check constraints are satisfied
        // For now, just resolve each generic argument type
        for (const TypePtr arg : type->genericArgs) {
            if (!resolveType(arg, ctx)) {
                // Error already reported
                return nullptr;
            }
        }
    }

    // ─── 4. Return the resolved type ─────────────────────────────────────
    // The named type is valid - return it as-is
    return const_cast<NamedTypeAST*>(type);
}

// =============================================================================
// resolveArrayType - Array types
// =============================================================================

TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve the element type
    TypeAST* element = resolveType(type->element, ctx);
    if (!element) {
        ctx.error(type, DiagCode::E3003, "invalid array element type");
        return nullptr;
    }

    // Check: Reference types cannot be stored in arrays (Downward Flow Rule)
    if (element->isa<RefTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "reference type (&T) cannot be stored in an array");
        return nullptr;
    }

    // Return the array type with resolved element type
    return const_cast<ArrayTypeAST*>(type);
}

// =============================================================================
// resolveNullableType - T?
// =============================================================================

TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve the inner type
    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid nullable inner type");
        return nullptr;
    }

    // Check: Function types cannot be nullable
    if (inner->isa<FuncTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "function types cannot be nullable");
        return nullptr;
    }

    // Check: Array types cannot be nullable
    if (inner->isa<ArrayTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "array types cannot be nullable (use empty array instead)");
        return nullptr;
    }

    return const_cast<NullableTypeAST*>(type);
}

// =============================================================================
// resolveFallibleType - T!
// =============================================================================

TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve the inner type
    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid fallible inner type");
        return nullptr;
    }

    // Check: Function types cannot be fallible
    if (inner->isa<FuncTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "function types cannot be fallible");
        return nullptr;
    }

    // Check: Array types cannot be fallible
    if (inner->isa<ArrayTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "array types cannot be fallible");
        return nullptr;
    }

    return const_cast<FallibleTypeAST*>(type);
}

// =============================================================================
// resolveCombinedType - T?!
// =============================================================================

TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve the inner type
    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid combined inner type");
        return nullptr;
    }

    // Check: Function types cannot be combined
    if (inner->isa<FuncTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "function types cannot be combined");
        return nullptr;
    }

    // Check: Array types cannot be combined
    if (inner->isa<ArrayTypeAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "array types cannot be combined");
        return nullptr;
    }

    return const_cast<CombinedTypeAST*>(type);
}

// =============================================================================
// resolveRefType - &T
// =============================================================================

TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve the inner type
    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid reference target type");
        return nullptr;
    }

    // ─── Validate Downward Flow Rule ──────────────────────────────────────
    // References (&T) are strictly scoped. They are allowed to flow downward
    // (into nested calls), but never upward or sideways.

    // 1. Cannot store &T in struct fields
    const TypeDeclAST* currentStruct = ctx.definingTypes.current();
    if (currentStruct && currentStruct->isa<StructDeclAST>()) {
        ctx.error(type, DiagCode::E3004,
                  "reference type (&T) cannot be stored in struct fields");
        return nullptr;
    }

    // 2. Cannot store &T in arrays (checked in resolveArrayType)
    // 3. Cannot return &T from functions (checked in resolveFuncType)
    //    For return types, we don't have the function context here,
    //    but the caller (resolveFuncType) will check.

    return const_cast<RefTypeAST*>(type);
}

// =============================================================================
// resolvePtrType - *T
// =============================================================================

TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Resolve the inner type
    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.error(type, DiagCode::E3003, "invalid pointer target type");
        return nullptr;
    }

    // Raw pointers are always valid (sealed conduit)
    // No additional validation needed

    return const_cast<PtrTypeAST*>(type);
}

// =============================================================================
// resolveFuncType - Function types
// =============================================================================

TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── 1. Resolve all parameters ────────────────────────────────────────
    for (ParamAST* param : type->params) {
        if (!resolveType(param->type, ctx)) {
            ctx.error(param, DiagCode::E3003, "invalid parameter type");
            return nullptr;
        }
    }

    // ─── 2. Resolve return type ───────────────────────────────────────────
    if (type->returnType) {
        TypeAST* returnType = resolveType(type->returnType, ctx);
        if (!returnType) {
            ctx.error(type, DiagCode::E3003, "invalid return type");
            return nullptr;
        }

        // ─── 3. Check: Cannot return reference type ──────────────────────
        // This is part of the Downward Flow Rule
        if (returnType->isa<RefTypeAST>()) {
            ctx.error(type, DiagCode::E3004,
                      "function cannot return reference type (&T)");
            return nullptr;
        }

        // ─── 4. If the return type is a function type, resolve it ────────
        if (returnType->isa<FuncTypeAST>()) {
            if (!resolveFuncType(returnType->as<FuncTypeAST>(), ctx)) {
                return nullptr;
            }
        }
    }

    return const_cast<FuncTypeAST*>(type);
}

// =============================================================================
// Type Compatibility Helpers
// =============================================================================

bool typesEqual(const TypeAST* a, const TypeAST* b) {
    // TODO: Implement proper type equality
    // For now, just compare kinds
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    // TODO: Check inner types for compound types
    return true;
}

bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx) {
    if (!target || !source) return false;

    // TODO: Implement proper assignability checking
    // This should handle:
    //   - Primitive type compatibility (int -> int32, etc.)
    //   - Nullable widening (T -> T?)
    //   - Fallible handling (T! -> T! but not T -> T!)
    //   - Struct/type alias compatibility

    // For now, just check if types are equal
    return typesEqual(target, source);
}

bool isNullableType(const TypeAST* type) {
    if (!type) return false;
    return type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>();
}

bool isFallibleType(const TypeAST* type) {
    if (!type) return false;
    return type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>();
}

TypeAST* unwrapNullable(TypeAST* type) {
    if (!type) return nullptr;
    if (type->isa<NullableTypeAST>()) {
        return type->as<NullableTypeAST>()->inner;
    }
    if (type->isa<CombinedTypeAST>()) {
        return type->as<CombinedTypeAST>()->inner;
    }
    return type;
}

TypeAST* unwrapFallible(TypeAST* type) {
    if (!type) return nullptr;
    if (type->isa<FallibleTypeAST>()) {
        return type->as<FallibleTypeAST>()->inner;
    }
    if (type->isa<CombinedTypeAST>()) {
        return type->as<CombinedTypeAST>()->inner;
    }
    return type;
}

bool isNumericType(const TypeAST* type) {
    // TODO: Implement numeric type check
    // Should handle int, float, double, decimal, and their fixed-width variants
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    // For now, return false for non-primitive types
    return false;
}

bool isIntegerType(const TypeAST* type) {
    // TODO: Implement integer type check
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    // For now, return false for non-primitive types
    return false;
}

bool isFloatType(const TypeAST* type) {
    // TODO: Implement float type check
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    // For now, return false for non-primitive types
    return false;
}

// =============================================================================
// Type Validation
// =============================================================================

bool validateConstFieldType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return false;

    if (isNullableType(type) || isFallibleType(type)) {
        ctx.error(type, DiagCode::E3004,
                  "const field cannot be nullable or fallible");
        return false;
    }

    return true;
}

bool validateTraitFieldType(const TypeAST* type, SemaContext& ctx) {
    // Trait fields can be nullable or fallible unless const
    // This is checked in analyzeTraitDecl
    return true;
}

bool validateRefContext(const RefTypeAST* type, SemaContext& ctx) {
    // Check Downward Flow Rule:
    // References can only appear as:
    //   - Function parameters
    //   - Local variable aliases

    // Struct fields are checked in resolveRefType
    // Array storage is checked in resolveArrayType
    // Function returns are checked in resolveFuncType

    return true;
}

} // namespace sema