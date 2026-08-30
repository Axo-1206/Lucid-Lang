/// @file SemaArena.hpp
/// @brief Semantic validation for the builtin Arena type.
/// 
/// This module validates Arena declarations and accesses during semantic analysis.
/// It uses the pure BuiltinTypes module for type detection and validation rules.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─── Arena Type Detection ──────────────────────────────────────────────────

/// @brief Check if a type is the builtin Arena type.
bool isArenaType(TypeAST* type);

/// @brief Check if a type is the builtin ArenaDescriptor type.
bool isArenaDescriptorType(TypeAST* type);

/// @brief Check if a NamedTypeAST refers to Arena.
bool isArenaNamedType(NamedTypeAST* named);

/// @brief Check if a NamedTypeAST refers to ArenaDescriptor.
bool isArenaDescriptorNamedType(NamedTypeAST* named);

/// @brief Check if a declaration is an Arena binding.
bool isArenaBinding(VarDeclAST* decl);

// ─── Arena Declaration Resolution ─────────────────────────────────────────

/// @brief Resolve and validate an Arena variable declaration.
/// 
/// Enforces:
///   1. Binding must be `const`
///   2. Initializer must be `Arena::create(size)` or `Arena::empty()`
/// 
/// @param decl The variable declaration to resolve.
/// @param ctx The semantic context.
/// @return true if valid, false if error reported.
bool resolveArenaVarDecl(VarDeclAST* decl, SemaContext& ctx);

// ─── Arena Access Resolution ──────────────────────────────────────────────

/// @brief Resolve and validate an Arena access expression.
/// 
/// Validates:
///   1. Method name is valid
///   2. Static vs instance form is correct
///   3. Generic arguments are valid (only alloc<T> can have them)
///   4. Argument count is correct for the method
///   5. LHS is a Arena binding (for instance methods)
/// 
/// @param expr The Arena access expression.
/// @param ctx The semantic context.
/// @return The resolved return type, or nullptr on error.
TypeAST* resolveArenaAccess(ArenaAccessExprAST* expr, SemaContext& ctx);

// ─── Arena Type Resolution ─────────────────────────────────────────────────

/// @brief Get or create the ArenaDescriptor type in the type cache.
/// 
/// @param ctx The semantic context.
/// @return The ArenaDescriptor NamedTypeAST.
TypeAST* getArenaDescriptorType(SemaContext& ctx);

/// @brief Get or create the Arena type in the type cache.
/// 
/// @param ctx The semantic context.
/// @return The Arena NamedTypeAST.
TypeAST* getArenaType(SemaContext& ctx);

// ─── Arena Initializer Validation ─────────────────────────────────────────

/// @brief Validate that an Arena initializer is valid.
/// 
/// @param init The initializer expression.
/// @param ctx The semantic context.
/// @return true if valid, false if error reported.
bool validateArenaInitializer(ExprAST* init, SemaContext& ctx);

} // namespace sema