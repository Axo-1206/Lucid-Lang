/// @file TypeCompat.cpp
/// @brief Type comparison, predicates, assignability, and validation.
/// 
/// @architectural_note Structural equality over TypeAST
///   These functions compare TypeAST nodes structurally (same shape,
///   same payload, recursively). No separate "resolved Type" exists yet.
/// 
/// @architectural_note Validation helpers
///   Additional helpers for const field validation, trait field validation,
///   reference scoping, and FFI compatibility.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// =============================================================================
// Structural Type Equality
// =============================================================================

/// @brief Structural equality of two TypeAST nodes.
/// 
/// Same shape, same payload, recursively. Not the same pointer.
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
            // All are "one wrapper, one inner type"
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

            // Compare hasArrow flag
            if (fa->hasArrow != fb->hasArrow) return false;

            // Compare parameters
            if (fa->params.size() != fb->params.size()) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                const ParamAST* pa = fa->params[i];
                const ParamAST* pb = fb->params[i];
                if (pa->isVariadic != pb->isVariadic) return false;
                if (pa->isConst != pb->isConst) return false;
                if (!typesEqual(pa->type, pb->type)) return false;
            }

            // ─── Compare return type (single) ────────────────────────────
            return typesEqual(fa->returnType, fb->returnType);
        }

        default:
            // Unknown type - can't prove equal
            return false;
    }
}

// =============================================================================
// Type Predicates
// =============================================================================

/// @brief True if type carries nil sentinel (T? or T?!).
bool isNullableType(const TypeAST* type) {
    return type && (type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>());
}

/// @brief True if type carries err sentinel (T! or T?!).
bool isFallibleType(const TypeAST* type) {
    return type && (type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>());
}

/// @brief True if type is a reference type (&T).
bool isReferenceType(const TypeAST* type) {
    return type && type->isa<RefTypeAST>();
}

/// @brief True if type is a raw pointer (*T).
bool isPointerType(const TypeAST* type) {
    return type && type->isa<PtrTypeAST>();
}

/// @brief True if type is a numeric type (integer or float).
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
            return false;
    }
}

/// @brief True if type is an integer type (not float).
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

/// @brief True if type is a float type.
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

// =============================================================================
// Type Unwrapping
// =============================================================================

/// @brief Strip one layer of ?/?!, return inner type.
TypeAST* unwrapNullable(TypeAST* type) {
    if (!type) return type;
    if (type->isa<NullableTypeAST>()) return type->as<NullableTypeAST>()->inner;
    if (type->isa<CombinedTypeAST>()) return type->as<CombinedTypeAST>()->inner;
    return type;
}

/// @brief Strip one layer of !/?!, return inner type.
TypeAST* unwrapFallible(TypeAST* type) {
    if (!type) return type;
    if (type->isa<FallibleTypeAST>()) return type->as<FallibleTypeAST>()->inner;
    if (type->isa<CombinedTypeAST>()) return type->as<CombinedTypeAST>()->inner;
    return type;
}

// =============================================================================
// Assignability
// =============================================================================

/// @brief Can a value of type `source` be used where `target` is required?
/// 
/// Widening rules:
///   1. Identical types → true
///   2. T → T? / T! / T?! → true (widening)
///   3. T? / T! → T?! → true (combining sentinels)
///   4. Everything else → false
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
        
        // T?! → T?! (already covered by typesEqual)
        return false;
    }

    // ─── 5. Nullable widening: T? → T?! ─────────────────────────────────
    if (target->isa<CombinedTypeAST>() && source->isa<NullableTypeAST>()) {
        const TypeAST* targetInner = target->as<CombinedTypeAST>()->inner;
        const TypeAST* sourceInner = source->as<NullableTypeAST>()->inner;
        return isAssignable(targetInner, sourceInner, ctx);
    }

    // ─── 6. Fallible widening: T! → T?! ─────────────────────────────────
    if (target->isa<CombinedTypeAST>() && source->isa<FallibleTypeAST>()) {
        const TypeAST* targetInner = target->as<CombinedTypeAST>()->inner;
        const TypeAST* sourceInner = source->as<FallibleTypeAST>()->inner;
        return isAssignable(targetInner, sourceInner, ctx);
    }

    // ─── 7. Trait conformance (Trait as target) ──────────────────────────
    // TODO: Implement trait conformance checking
    // If target is a NamedTypeAST that resolves to a trait, check if source
    // implements that trait.
    (void)ctx;

    return false;
}

// =============================================================================
// Type Validation
// =============================================================================

/// @brief Validate that a const field's type is not nullable or fallible.
/// 
/// From DeclAST.hpp:
///   A const field may NOT be nullable (T?) or fallible (T!).
bool validateConstFieldType(const TypeAST* type, SemaContext& ctx) {
    if (isNullableType(type) || isFallibleType(type)) {
        ctx.error(type, DiagCode::E3004,
                  "const field cannot be nullable or fallible");
        return false;
    }
    return true;
}

/// @brief Validate that a trait field is not nullable or fallible.
/// 
/// From DeclAST.hpp:
///   Trait fields must not be nullable or fallible.
bool validateTraitFieldType(const TypeAST* type, SemaContext& ctx) {
    if (isNullableType(type) || isFallibleType(type)) {
        ctx.error(type, DiagCode::E3004,
                  "trait field cannot be nullable or fallible");
        return false;
    }
    return true;
}

/// @brief Validate reference type context (Downward Flow Rule).
/// 
/// From TypeAST.hpp:
///   References (&T) can only appear as:
///     - Function parameters
///     - Local variable aliases
/// 
///   Invalid contexts:
///     - Struct fields (infinite size)
///     - Array/Slice storage
///     - Function returns
bool validateRefContext(const RefTypeAST* type, SemaContext& ctx) {
    const TypeDeclAST* currentType = ctx.definingTypes.current();
    
    // Check if we're inside a struct field being defined
    if (currentType && ctx.definingTypes.isDefining(currentType)) {
        ctx.error(type, DiagCode::E3004,
                   "reference type (&T) cannot be stored in a struct field");
        return false;
    }

    // TODO: Check if we're in a function return position
    // TODO: Check if we're in an array element type
    
    return true;
}

// =============================================================================
// FFI Compatibility
// =============================================================================

/// @brief True if type is legal at an FFI boundary.
/// 
/// FFI-compatible types:
///   - Primitive types (int, float, bool, etc.)
///   - Raw pointers (*T)
///   - Void (no return type)
///   - Structs with FFI-compatible fields
///   - Enums with FFI-compatible backing types
bool isValidFFIType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return true; // void

    // Primitives are always valid
    if (type->isa<PrimitiveTypeAST>()) return true;

    // Raw pointers are valid
    if (type->isa<PtrTypeAST>()) {
        return isValidFFIType(type->as<PtrTypeAST>()->inner, ctx);
    }

    // References are NOT valid
    if (type->isa<RefTypeAST>()) return false;

    // Named types (structs, enums) must be FFI-compatible
    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        const TypeDeclAST* decl = lookupType(named->name, ctx);
        if (!decl) return false;

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

    // Arrays are valid if element type is FFI-compatible
    if (type->isa<ArrayTypeAST>()) {
        // Must be fixed-size or pointer to array
        return isValidFFIType(type->as<ArrayTypeAST>()->element, ctx);
    }

    // Nullable types are NOT valid
    if (type->isa<NullableTypeAST>() ||
        type->isa<FallibleTypeAST>() ||
        type->isa<CombinedTypeAST>()) {
        return false;
    }

    // Function types are NOT valid
    if (type->isa<FuncTypeAST>()) return false;

    return false;
}

} // namespace sema