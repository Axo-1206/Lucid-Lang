/// @file codegen/support/GenericMangledName.hpp
/// @brief Mangled name generation for generic instantiations ONLY.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file generates mangled names for generic INSTANTIATIONS (specialized
/// versions of generic functions/structs with concrete type arguments).
///
/// ─── Why Separate from Sema's Mangling? ─────────────────────────────────────
/// Sema generates mangled names for ALL declarations during semantic analysis.
/// However, generic instantiations are discovered lazily during CodeGen when
/// the generic is actually used. Sema cannot know all instantiations ahead of
/// time (especially with cross-module usage), so CodeGen generates mangled
/// names for each concrete instantiation when it's first encountered.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// CodeGenGeneric.cpp calls generateMangledNameForGeneric() when creating
/// specialized functions/structs via monomorphization (@[specialize]).
///
/// ─── What This File Does NOT Do ───────────────────────────────────────────
/// - Does NOT generate mangled names for non-generic declarations (Sema does that)
/// - Does NOT generate mangled names for generic templates (Sema does that)
/// - Only generates names for CONCRETE instantiations: identity<int>, Box<float>, etc.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "../context/CodeGenContext.hpp"
#include "GenericSubstitution.hpp"

#include <string>
#include <vector>

namespace codegen {

/// @brief Generate a mangled name for a generic instantiation.
/// @param baseDecl The generic declaration (function or struct).
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The mangled name as an InternedString.
InternedString generateMangledNameForGeneric(
    DeclAST* baseDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

// ─── Core Encoding Functions ──────────────────────────────────────────────

/// @brief Encode a type to a mangled string.
/// @param type The type to encode.
/// @param pool The string pool for looking up names.
/// @param subst Optional substitution context. When non-null, any
///        `NamedTypeAST` matching one of the substitution's generic
///        parameters is replaced with its concrete type argument BEFORE
///        encoding - at any depth (top-level, or nested inside `*T`, `T?`,
///        `Array<T>`, `Box<T>`, etc). Without this, two different concrete
///        instantiations of the same generic can mangle to the identical
///        string whenever the generic parameter only appears nested inside
///        a composite type, since the plain (non-substituted) type would
///        just be encoded as the literal parameter name "T" either way.
/// @return The encoded type string (as std::string for building).
std::string typeToMangleString(
    TypeAST* type,
    StringPool& pool,
    const GenericSubstitution* subst = nullptr
);

/// @brief Sanitize a string for use in a mangled name.
/// @param str The string to sanitize.
/// @return The sanitized string.
std::string sanitizeForMangledName(const std::string& str);

/// @brief Get the module path for mangling.
/// @param ctx The code generation context.
/// @return The sanitized module path.
std::string getMangledModulePath(CodeGenContext& ctx);

// ─── Primitive Type Encoding ─────────────────────────────────────────────

/// @brief Encode a primitive kind to a single character.
/// @param kind The primitive kind.
/// @return The encoded character.
char encodePrimitiveKind(PrimitiveKind kind);

/// @brief Check if a type is a primitive type.
bool isPrimitiveType(TypeAST* type);

} // namespace codegen