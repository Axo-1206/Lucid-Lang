/// @file sema/support/MangledName.hpp
/// @brief Mangled name generation for declarations.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "../context/SemaContext.hpp"
#include "../context/Generic.hpp"

#include <string>

namespace sema {

// ─── Declaration Mangling ─────────────────────────────────────────────────

/// @brief Generate a mangled name for a function declaration.
InternedString generateMangledName(FuncDeclAST* decl, SemaContext& ctx);

/// @brief Generate a mangled name for a variable declaration.
InternedString generateMangledName(VarDeclAST* decl, SemaContext& ctx);

/// @brief Generate a mangled name for a struct declaration.
InternedString generateMangledName(StructDeclAST* decl, SemaContext& ctx);

/// @brief Generate a mangled name for an enum declaration.
InternedString generateMangledName(EnumDeclAST* decl, SemaContext& ctx);

// ─── Generic Instantiation Mangling ──────────────────────────────────────

/// @brief Generate a full mangled name for a generic instantiation.
/// 
/// This is the PRIMARY entry point for generic instantiation mangling.
/// It generates the full symbol name including:
///   - Module path
///   - Declaration name
///   - Generic parameters (the template params)
///   - Concrete type arguments
///   - Parameter types (substituted)
///   - Return type (substituted)
///   - Field types for structs (substituted)
///
/// @param decl The generic declaration (function or struct).
/// @param typeArgs The concrete type arguments.
/// @param ctx The semantic context.
/// @return The full mangled name as an InternedString.
InternedString generateMangledNameForGeneric(
    DeclAST* decl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx
);

/// @brief Generate a mangled name for a generic instantiation from a base name.
/// 
/// This is a CONVENIENCE helper that extends an already-mangled base name
/// with generic arguments. Used internally or for simple cases where the
/// full declaration context isn't available.
///
/// @param baseName The already-mangled base name.
/// @param typeArgs The concrete type arguments.
/// @param ctx The semantic context.
/// @return The extended mangled name.
InternedString extendMangledNameWithGenericArgs(
    InternedString baseName,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx
);

// ─── Core Encoding Functions ─────────────────────────────────────────────

/// @brief Encode a type to a mangled string.
std::string typeToMangleString(TypeAST* type, SemaContext& ctx);

/// @brief Encode a type to a mangled string with optional substitution.
/// 
/// @note This overload uses GenericSubstitution as a const pointer.
///       The full definition is only required in MangledName.cpp.
std::string typeToMangleString(TypeAST* type, SemaContext& ctx, const GenericSubstitution* subst);

/// @brief Sanitize a string for use in a mangled name.
std::string sanitizeForMangledName(const std::string& str);

/// @brief Get the module path for mangling.
std::string getMangledModulePath(SemaContext& ctx);

/// @brief Encode a primitive kind to a single character.
char encodePrimitiveKind(PrimitiveKind kind);

} // namespace sema