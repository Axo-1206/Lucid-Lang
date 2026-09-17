/// @file sema/support/MangledName.hpp
/// @brief Mangled name generation for declarations.
///
/// ─── Export Behavior ─────────────────────────────────────────────────────
///
/// A declaration carrying `@[export]` has `decl->isExported == true` (set by
/// Sema's attribute validator) and is mangled to exactly its source name:
/// no module path, no signature suffix. This is what makes the symbol
/// `main` from `@[export] const main fn () -> int` findable by the C runtime
/// and by `InterpreterProgram::run`'s entry-point lookup.
///
/// Export-aware mangling currently applies to functions and variables only.
/// Structs and enums deliberately keep their fully-mangled names even when
/// `@[export]`ed, because their mangled name is used as the LLVM type name
/// and the module-path prefix is what prevents two modules' `enum Status`
/// from colliding. See the individual overloads in MangledName.cpp.

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
///
/// If `decl->isExported`, returns `decl->name` unchanged.
/// Otherwise returns the full `_L<module>_<name>_P<params>_R<return>` form.
///
/// For a generic function template, this produces the *template's* name.
/// Export-aware mangling returns the source name for the template; each
/// specialization still receives its own mangled name from
/// generateMangledNameForGeneric. The template itself is never emitted to
/// LLVM IR, so an exported generic function's source-named symbol does not
/// exist at runtime — a limitation worth a diagnostic if `@[export]` on a
/// generic function ever becomes a real use case.
InternedString generateMangledName(FuncDeclAST* decl, SemaContext& ctx);

/// @brief Generate a mangled name for a variable declaration.
///
/// If `decl->isExported`, returns `decl->name` unchanged.
InternedString generateMangledName(VarDeclAST* decl, SemaContext& ctx);

/// @brief Generate a mangled name for a struct declaration.
///
/// Does NOT honor `decl->isExported` — see the file-level comment.
InternedString generateMangledName(StructDeclAST* decl, SemaContext& ctx);

/// @brief Generate a mangled name for an enum declaration.
///
/// Does NOT honor `decl->isExported` — see the file-level comment.
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
/// Does NOT consult `isExported`. A specialization is a distinct symbol
/// and must remain distinct; the template's exported-ness is irrelevant
/// to the specialization's name.
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

// ─── Core Encoding Functions ─────────────────────────────────────────────

/// @brief Encode a type to a mangled string.
std::string typeToMangleString(TypeAST* type, SemaContext& ctx);

/// @brief Sanitize a string for use in a mangled name.
std::string sanitizeForMangledName(const std::string& str);

/// @brief Get the module path for mangling.
std::string getMangledModulePath(SemaContext& ctx);

/// @brief Encode a primitive kind to a single character.
char encodePrimitiveKind(PrimitiveKind kind);

} // namespace sema