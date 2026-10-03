/// @file SemaTypePredicates.cpp
/// @brief Type predicates and small type queries.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include <unordered_set>

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Primitive predicates
// ─────────────────────────────────────────────────────────────────────────────

bool isPrimitiveType(TypeAST* type) {
    return type && type->isa<PrimitiveTypeAST>();
}

bool isBoolType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
}

bool isCharType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Char;
}

bool isStringType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String;
}

bool isUnitType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Unit;
}

bool isIntegerType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return isIntegerKind(type->as<PrimitiveTypeAST>()->primitiveKind);
}

bool isFloatType(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return isFloatKind(type->as<PrimitiveTypeAST>()->primitiveKind);
}

bool isNumericType(TypeAST* type) {
    return isIntegerType(type) || isFloatType(type);
}

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper / structural predicates
// ─────────────────────────────────────────────────────────────────────────────

bool isNullableType(TypeAST* type) {
    return type && type->isa<NullableTypeAST>();
}

bool isArrayType(TypeAST* type) {
    return type && type->isa<ArrayTypeAST>();
}

bool isRowRefType(TypeAST* type) {
    return type && type->isa<RowRefTypeAST>();
}

bool isFunctionType(TypeAST* type) {
    return type && type->isa<FunctionTypeAST>();
}

bool isNamedType(TypeAST* type) {
    return type && type->isa<NamedTypeAST>();
}

// ─────────────────────────────────────────────────────────────────────────────
// Table predicates — need the context to follow the NamedTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// True if `type` names a table (columned or host-backed).
bool isTableType(TypeAST* type, SemaContext& ctx) {
    return isColumnedTableType(type, ctx) || isHostBackedTableType(type, ctx);
}

bool isColumnedTableType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    if (!named->resolvedDecl) {
        // The type was never resolved. Rather than mutate here — a
        // predicate should not have a side effect — return false and let
        // the caller either resolve first or accept "not a table".
        return false;
    }
    TableDeclAST* table = named->resolvedDecl->as<TableDeclAST>();
    return !table->isHostBacked;
}

bool isHostBackedTableType(TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    if (!named->resolvedDecl) {
        return false;
    }
    TableDeclAST* table = named->resolvedDecl->as<TableDeclAST>();
    return table->isHostBacked;
}

// ─────────────────────────────────────────────────────────────────────────────
// Unwrapping
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* unwrapNullable(TypeAST* type) {
    if (!type) return type;
    if (type->isa<NullableTypeAST>()) {
        return type->as<NullableTypeAST>()->inner;
    }
    return type;
}

// ─────────────────────────────────────────────────────────────────────────────
// Numeric helpers
// ─────────────────────────────────────────────────────────────────────────────

size_t getNumericBitWidth(TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return 0;
    PrimitiveKind kind = type->as<PrimitiveTypeAST>()->primitiveKind;
    if (!isNumericKind(kind)) return 0;
    return primitiveBitWidth(kind);
}

TypeAST* getLargerNumericType(TypeAST* a, TypeAST* b, SemaContext& ctx) {
    if (!a || !b) return nullptr;
    if (!isNumericType(a) || !isNumericType(b)) return nullptr;

    // A float on either side wins — the result is a float.
    if (isFloatType(a) || isFloatType(b)) {
        if (isFloatType(a) && isFloatType(b)) {
            // Both float: take the wider.
            return getNumericBitWidth(a) >= getNumericBitWidth(b) ? a : b;
        }
        // Exactly one is a float: the float type is the result.
        return isFloatType(a) ? a : b;
    }

    // Both integers: take the wider.
    return getNumericBitWidth(a) >= getNumericBitWidth(b) ? a : b;
}

// ─────────────────────────────────────────────────────────────────────────────
// Primitive-name lookup
// ─────────────────────────────────────────────────────────────────────────────

bool isPrimitiveTypeName(InternedString name, StringPool& pool) {
    std::string_view view = pool.lookupView(name);

    // The primitive type keywords (§2.2). Sized aliases fold to the same
    // PrimitiveKind as their canonical spelling.
    static const std::unordered_set<std::string_view> kPrimitiveNames = {
        "bool", "char", "string", "unit",
        "int8", "int16", "int32", "int64",
        "uint8", "uint16", "uint32", "uint64",
        "float32", "float64",
        // Sized aliases
        "int", "long", "uint", "ulong", "float", "double",
    };
    return kPrimitiveNames.find(view) != kPrimitiveNames.end();
}

PrimitiveKind primitiveKindFromName(InternedString name, StringPool& pool) {
    std::string_view view = pool.lookupView(name);

    // Canonical forms
    if (view == "bool")    return PrimitiveKind::Bool;
    if (view == "char")    return PrimitiveKind::Char;
    if (view == "string")  return PrimitiveKind::String;
    if (view == "unit")    return PrimitiveKind::Unit;

    if (view == "int8")    return PrimitiveKind::Int8;
    if (view == "int16")   return PrimitiveKind::Int16;
    if (view == "int32")   return PrimitiveKind::Int32;
    if (view == "int64")   return PrimitiveKind::Int64;

    if (view == "uint8")   return PrimitiveKind::Uint8;
    if (view == "uint16")  return PrimitiveKind::Uint16;
    if (view == "uint32")  return PrimitiveKind::Uint32;
    if (view == "uint64")  return PrimitiveKind::Uint64;

    if (view == "float32") return PrimitiveKind::Float32;
    if (view == "float64") return PrimitiveKind::Float64;

    // Sized aliases — these fold to the canonical kind, per §2.2.
    if (view == "int")     return PrimitiveKind::Int32;
    if (view == "long")    return PrimitiveKind::Int64;
    if (view == "uint")    return PrimitiveKind::Uint32;
    if (view == "ulong")   return PrimitiveKind::Uint64;
    if (view == "float")   return PrimitiveKind::Float32;
    if (view == "double")  return PrimitiveKind::Float64;

    // Precondition: the caller checked with isPrimitiveTypeName.
    AST_ASSERT_MSG(false,
        "primitiveKindFromName: called with a name that is not a "
        "primitive type name");
    return PrimitiveKind::Int32;   // unreachable in debug; sentinel in release
}

} // namespace lucid::sema