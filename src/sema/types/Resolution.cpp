/// @file Resolution.cpp
/// @brief Implementation of type resolution.

#include "SemaType.hpp"
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

} // namespace sema