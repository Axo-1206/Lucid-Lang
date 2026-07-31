/// @file SemaResolve.hpp
/// @brief Type resolution - converts type AST nodes to semantic representations.
/// 
/// Resolution uses lookup to find declarations, then builds the complete
/// semantic type representation. This is where generic arguments are resolved,
/// and self-references are detected.
/// 
/// @resolve_priority
///   1. Check if it's a generic parameter (highest priority)
///   2. Look up as concrete type (local scopes → module scope)
///   3. Resolve generic arguments if present
///   4. Validate type-specific rules (Downward Flow, etc.)
/// 
/// @example
///   // Resolve a type annotation
///   TypeAST* resolved = resolveType(typeNode, ctx);
///   if (!resolved) {
///       // Error already reported
///       return;
///   }

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─── Main Resolution Entry Point ─────────────────────────────────────────

/// @brief Resolve a type annotation to its semantic representation.
/// 
/// This is the main entry point for type resolution. It dispatches to
/// the appropriate resolver based on the type node's kind.
/// 
/// @param type The type annotation to resolve.
/// @param ctx The semantic context.
/// @return The resolved TypeAST, or nullptr on failure.
/// 
/// @note On failure, an error is reported via ctx.error().
/// 
/// @example
///   TypeAST* resolved = resolveType(decl->type, ctx);
///   if (!resolved) return; // Error already reported
TypeAST* resolveType(const TypeAST* type, SemaContext& ctx);

// ─── Specific Type Resolvers ─────────────────────────────────────────────

/// @brief Resolve a primitive type (always succeeds).
/// 
/// Primitive types are built-in and always valid.
TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx);

/// @brief Resolve a named type with priority.
/// 
/// Resolution priority:
///   1. Check if it's a generic parameter (highest priority)
///   2. Look up as concrete type in scopes
///   3. Validate generic arguments if present
///   4. Check for self-reference (if being defined)
/// 
/// @param type The named type to resolve.
/// @param ctx The semantic context.
/// @return The resolved NamedTypeAST, or nullptr on failure.
/// 
/// @see SemaLookup.hpp - lookupType() for the actual lookup
/// @see SemaValidate.hpp - validateGenericArguments()
TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx);

/// @brief Resolve an array type.
/// 
/// Recursively resolves the element type and validates that it's not
/// a reference type (Downward Flow Rule).
/// 
/// @param type The array type to resolve.
/// @param ctx The semantic context.
/// @return The resolved ArrayTypeAST, or nullptr on failure.
TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx);

/// @brief Resolve a nullable type (T?).
/// 
/// Recursively resolves the inner type and validates that function types
/// and array types cannot be nullable.
/// 
/// @param type The nullable type to resolve.
/// @param ctx The semantic context.
/// @return The resolved NullableTypeAST, or nullptr on failure.
TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx);

/// @brief Resolve a fallible type (T!).
/// 
/// Recursively resolves the inner type and validates that function types
/// and array types cannot be fallible.
/// 
/// @param type The fallible type to resolve.
/// @param ctx The semantic context.
/// @return The resolved FallibleTypeAST, or nullptr on failure.
TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx);

/// @brief Resolve a combined type (T?!).
/// 
/// Recursively resolves the inner type and validates that function types
/// and array types cannot be combined.
/// 
/// @param type The combined type to resolve.
/// @param ctx The semantic context.
/// @return The resolved CombinedTypeAST, or nullptr on failure.
TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx);

/// @brief Resolve a reference type (&T).
/// 
/// Recursively resolves the inner type and checks the Downward Flow Rule:
///   - Cannot store &T in struct fields
///   - Cannot store &T in arrays
///   - Cannot return &T from functions
/// 
/// @param type The reference type to resolve.
/// @param ctx The semantic context.
/// @return The resolved RefTypeAST, or nullptr on failure.
/// 
/// @see SemaValidate.hpp - validateRefContext()
TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx);

/// @brief Resolve a pointer type (*T).
/// 
/// Recursively resolves the inner type. Raw pointers are always valid
/// (sealed conduit model).
/// 
/// @param type The pointer type to resolve.
/// @param ctx The semantic context.
/// @return The resolved PtrTypeAST, or nullptr on failure.
TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx);

/// @brief Resolve a function type.
/// 
/// Recursively resolves all parameters and the return type.
/// Validates that return types cannot be reference types or trait types.
/// 
/// @param type The function type to resolve.
/// @param ctx The semantic context.
/// @return The resolved FuncTypeAST, or nullptr on failure.
TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx);

// ─── Trait Resolution ────────────────────────────────────────────────────

/// @brief Resolve a trait reference to its declaration.
/// 
/// A trait reference is a NamedTypeAST that must resolve to a TraitDeclAST.
/// Used in:
///   - Struct declarations: `struct Entity : Vector2, Named { ... }`
///   - Generic constraints: `<T : Vector2 + Named>`
/// 
/// @param ref The trait reference (NamedTypeAST).
/// @param ctx The semantic context.
/// @return The resolved TraitDeclAST, or nullptr on failure.
const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx);

// ─── Self-Reference Detection ───────────────────────────────────────────

/// @brief Check if a let initializer references the variable being declared.
/// 
/// Prevents cases like: `let x int = x` (x is used before initialization).
/// This is a special case because the variable name is already registered
/// in Phase 1, so normal lookup would succeed.
/// 
/// @param expr The initializer expression to check.
/// @param varName The name of the variable being declared.
/// @param ctx The semantic context.
/// 
/// @example
///   // In resolveVarDecl:
///   if (decl->keyword == DeclKeyword::Let) {
///       checkLetSelfReference(decl->init, decl->name, ctx);
///   }
void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx);

} // namespace sema