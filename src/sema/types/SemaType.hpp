/// @file SemaType.hpp
/// @brief Name lookup for the semantic analyzer.
/// 
/// All name lookup logic with proper priority and diagnostics.
/// 
/// @architectural_note Lookup Priority
///   1. Generic parameters in current scope (highest priority, shadow everything)
///   2. Value/Type declarations in local scopes (innermost to outermost)
///   3. Value/Type declarations in module scope (global)
/// 
/// @architectural_note Two namespaces
///   - VALUE NAMESPACE: variables, functions, parameters, fields, enum variants
///   - TYPE NAMESPACE: structs, enums, traits, generic params
///
/// Resolves type annotations to their semantic representations.
/// 
/// @architectural_note Types are read-only
///   The parser created all TypeAST nodes. This file resolves them by
///   looking up names in the symbol table and validating compound types.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Self reference check
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a let initializer references the variable being declared.
/// We need to prevent case: let x int = x
/// Because x was never initialized, so this is a critical error
void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameter Lookup
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a name is a generic parameter in the current scope.
/// 
/// Generic parameters have the HIGHEST priority and shadow type names.
bool isGenericParam(InternedString name, SemaContext& ctx);

/// @brief Look up a generic parameter by name.
/// @return The GenericParamDeclAST if found, nullptr otherwise.
const GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Value Lookup (variables, functions, parameters, fields, enum variants)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Look up a value declaration by name.
/// 
/// Searches: generic params → local scopes → module scope
/// Generic params are NOT values, so they don't match here.
const ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx);

/// @brief Look up a value and report E2001 if not found.
const ValueDeclAST* resolveValueOrError(const IdentifierExprAST* expr, SemaContext& ctx);

/// @brief Look up a function by name.
/// 
/// Convenience wrapper that checks the resolved value is a FuncDeclAST.
const FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type Lookup (structs, enums, traits)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Look up a type declaration by name.
/// 
/// Searches: local scopes → module scope
/// Generic parameters are NOT type declarations (they shadow, but are separate).
const TypeDeclAST* lookupType(InternedString name, SemaContext& ctx);

/// @brief Look up a type with proper priority (generic params shadow types).
/// 
/// This is the main type resolution function. It handles:
///   1. Check if it's a generic parameter (returns nullptr, no error)
///   2. Look up as concrete type (returns TypeDeclAST*)
///   3. Not found (reports E2002, returns nullptr)
const TypeDeclAST* resolveTypeOrError(const NamedTypeAST* type, SemaContext& ctx);

/// @brief Resolve a named type reference, reporting E2002 on failure.
/// Alias for resolveTypeOrError() for consistency.
const TypeDeclAST* resolveTypeNameOrError(const NamedTypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Trait Reference Resolution
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a trait reference to its declaration.
///
/// A trait reference is a NamedTypeAST that must resolve to a TraitDeclAST.
/// This is used in:
///   - Struct declarations: `struct Entity : Vector2, Named { ... }`
///   - Generic constraints: `<T : Vector2 + Named>`
///
/// @param ref The trait reference (NamedTypeAST).
/// @param ctx The semantic context.
/// @return The resolved TraitDeclAST, or nullptr on error.
const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Module Member Lookup (module:member)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Look up a member in a module's table.
/// Used for module:member access. The module must already be resolved.
const ValueDeclAST* lookupModuleMember(ModuleAST* module, InternedString memberName, SemaContext& ctx);

/// @brief Resolve a module alias and look up a member, with error reporting.
const ValueDeclAST* resolveModuleMemberOrError(ModuleAccessExprAST* access, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Callee Resolution (for function calls)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a call expression's callee to the FuncDeclAST it names.
/// 
/// Handles two callee shapes:
///   - IdentifierExprAST: Look up in value namespace
///   - ModuleAccessExprAST: Look up module alias, then member
/// 
/// Any other callee shape (curried call, function literal) returns nullptr
/// silently - the caller must check the callee's resolved type instead.
const FuncDeclAST* resolveCalleeOrError(const ExprAST* callee, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Redeclaration Checkers - Check only the current tier (not outer scopes)
// ─────────────────────────────────────────────────────────────────────────────

bool isValueRedeclared(InternedString name, SemaContext& ctx);
bool isTypeRedeclared(InternedString name, SemaContext& ctx);
bool isGenericParamRedeclared(InternedString name, SemaContext& ctx);
bool isImportAliasRedeclared(InternedString alias, SemaContext& ctx);

bool reportValueRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportTypeRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportGenericParamRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportImportAliasRedeclaration(InternedString alias, const BaseAST* node, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type Resolution Entry Point
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a type annotation.
/// 
/// For NamedTypeAST: LOOKUP the name.
/// For compound types: recursively resolve inner types.
/// 
/// The parser already created all TypeAST nodes. This just validates they exist.
TypeAST* resolveType(const TypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Specific Type Resolvers
// ─────────────────────────────────────────────────────────────────────────────

/// Primitive types are always valid (built-in).
TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx);

/// @brief Resolve a named type.
/// 
/// LOOKUP PRIORITY (highest to lowest):
///   1. Generic parameter in current scope
///   2. Type in local scopes
///   3. Type in module scope (fallback)
/// 
/// Reports E2002 if not found in any tier.
TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx);

/// Recursively resolve array element type.
TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx);

/// Recursively resolve inner type.
TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx);
TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx);
TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx);

/// @brief Resolve reference type.
/// 
/// Checks Downward Flow Rule:
///   - Cannot store &T in struct fields
///   - Cannot store &T in arrays
///   - Cannot return &T from functions
TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx);

/// Resolve pointer type - always valid (sealed conduit).
TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx);

/// Recursively resolve parameter and return types.
TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type Compatibility Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Structural equality of two types.
bool typesEqual(const TypeAST* a, const TypeAST* b);

/// True if source value can be used where target is expected.
bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

/// Strip ?/?!, return inner type.
TypeAST* unwrapNullable(TypeAST* type);
TypeAST* unwrapFallible(TypeAST* type);

// ─────────────────────────────────────────────────────────────────────────────
// Type Validation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate that a const field's type is not nullable or fallible.
bool validateConstFieldType(const TypeAST* type, SemaContext& ctx);

/// @brief Validate that a trait field is not nullable or fallible.
bool validateTraitFieldType(const TypeAST* type, SemaContext& ctx);

/// @brief Validate reference type context (Downward Flow Rule).
bool validateRefContext(const RefTypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type Predicates (inline)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a type is a boolean type.
inline bool isBoolType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Bool;
}

/// @brief Check if a type is an integer type.
inline bool isIntegerType(const TypeAST* type) {
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

/// @brief Check if a type is a float type.
inline bool isFloatType(const TypeAST* type) {
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

/// @brief Check if a type is a numeric type (integer or float).
inline bool isNumericType(const TypeAST* type) {
    return isIntegerType(type) || isFloatType(type);
}

/// @brief Check if a type is a string type.
inline bool isStringType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String;
}

/// @brief Check if a type is a char type.
inline bool isCharType(const TypeAST* type) {
    if (!type || !type->isa<PrimitiveTypeAST>()) return false;
    return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Char;
}

/// @brief True if type carries nil sentinel (T? or T?!).
inline bool isNullableType(const TypeAST* type) {
    return type && (type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>());
}

/// @brief True if type carries err sentinel (T! or T?!).
inline bool isFallibleType(const TypeAST* type) {
    return type && (type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>());
}

/// @brief True if type is a reference type (&T).
inline bool isReferenceType(const TypeAST* type) {
    return type && type->isa<RefTypeAST>();
}

/// @brief True if type is a raw pointer (*T).
inline bool isPointerType(const TypeAST* type) {
    return type && type->isa<PtrTypeAST>();
}

/// @brief Check if a type is an enum type.
/// 
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type is an enum type.
inline bool isEnumType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = lookupType(named->name, ctx);
    return decl && decl->isa<EnumDeclAST>();
}


/// @brief Check if a type is valid for switch statements.
/// 
/// Valid switch types:
///   - Integer types (int, uint, int8, uint8, etc.)
///   - Bool
///   - Char
///   - String
///   - Enum types
/// 
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type is valid for switch.
inline bool isValidSwitchType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return false;

    // Integer types are valid
    if (isIntegerType(type)) return true;

    // Bool is valid
    if (isBoolType(type)) return true;

    // Char is valid
    if (isCharType(type)) return true;

    // String is valid
    if (isStringType(type)) return true;

    // Enum types are valid (NamedType resolving to EnumDeclAST)
    if (isEnumType(type, ctx)) return true;

    return false;
}

/// @brief Get the enum declaration from a type.
/// 
/// @param type The type (must be an enum type).
/// @param ctx The semantic context.
/// @return The EnumDeclAST, or nullptr if not an enum.
inline const EnumDeclAST* getEnumDeclFromType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return nullptr;
    
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    const TypeDeclAST* decl = lookupType(named->name, ctx);
    if (!decl || !decl->isa<EnumDeclAST>()) return nullptr;
    
    return decl->as<EnumDeclAST>();
}

/// @brief Check if a value is compatible with a switch subject type.
/// 
/// For enum types: the value must be a variant of that enum.
/// For integer types: the value must be an integer literal.
/// For bool: the value must be true/false.
/// For char: the value must be a char literal.
/// For string: the value must be a string literal.
/// 
/// @param value The case value expression.
/// @param subjectType The switch subject type.
/// @param ctx The semantic context.
/// @return true if the value is compatible.
inline bool isSwitchCaseCompatible(const ExprAST* value, const TypeAST* subjectType, SemaContext& ctx) {
    if (!value || !subjectType) return false;

    // ─── Enum type: value must be an enum variant ──────────────────────
    if (isEnumType(subjectType, ctx)) {
        // Value must be a field access (EnumName.Variant)
        if (!value->isa<FieldAccessExprAST>()) {
            return false;
        }
        const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
        
        // Object must be an identifier resolving to the enum type
        if (!field->object->isa<IdentifierExprAST>()) {
            return false;
        }
        const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
        
        const TypeDeclAST* decl = lookupType(id->name, ctx);
        if (!decl || !decl->isa<EnumDeclAST>()) {
            return false;
        }
        const EnumDeclAST* enumDecl = decl->as<EnumDeclAST>();
        
        // Check if the variant exists in the enum
        for (const EnumVariantAST* variant : enumDecl->variants) {
            if (variant->name == field->fieldName) {
                return true;
            }
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



/// @brief Check if a type is a trait.
inline bool isTrait(InternedString name, SemaContext& ctx) {
    const TypeDeclAST* decl = lookupType(name, ctx);
    return decl && decl->isa<TraitDeclAST>();
}

/// @brief Check if a type is a struct.
inline bool isStruct(InternedString name, SemaContext& ctx) {
    const TypeDeclAST* decl = lookupType(name, ctx);
    return decl && decl->isa<StructDeclAST>();
}

/// @brief Check if a type is an enum.
inline bool isEnum(InternedString name, SemaContext& ctx) {
    const TypeDeclAST* decl = lookupType(name, ctx);
    return decl && decl->isa<EnumDeclAST>();
}

/// @brief Check if a type is a trait type.
/// 
/// A trait type is a NamedTypeAST that resolves to a TraitDeclAST.
inline bool isTraitType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    return isTrait(named->name, ctx);
}

/// @brief Check if a type is a struct type.
inline bool isStructType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    return isStruct(named->name, ctx);
}

/// @brief Check if a type is a generic parameter.
inline bool isGenericParamType(const TypeAST* type, SemaContext& ctx) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = type->as<NamedTypeAST>();
    return isGenericParam(named->name, ctx);
}

} // namespace sema