/// @file MangledName.hpp
/// @brief Mangled name generation for declarations.
///
/// ─── What this file produces ──────────────────────────────────────────────
/// Every top-level declaration that survives to the bytecode module gets
/// a symbol name. The name is what the bytecode's symbol table stores,
/// what `HostSymbolTable` maps to native addresses, and what a future
/// `.lucb` serializer writes into the artifact.
///
/// Two shapes:
///
///   - **An `@export`ed declaration** gets its source name verbatim.
///     This is the whole point of `@export`: the host knows a function
///     as `main` or `onTick`, and the symbol the compiled artifact
///     exposes for it must be exactly that name. No prefix, no
///     signature encoding, no module path.
///
///   - **A non-exported declaration** gets a mangled name of the form
///     `_L<module-path>_<name>`. The module path is the file path
///     relative to the package root, with `/`, `\`, and `.` folded to
///     `_`. The result is unique across the loaded module set: two
///     modules each declaring a private `helper` produce
///     `_Lfoo_bar_helper` and `_Lbaz_qux_helper`, and the two do not
///     collide.
///
/// ─── What this file does NOT do (vs. the previous design) ─────────────────
/// No signature encoding. The old grammar needed `_P<params>_R<return>`
/// on a function's mangled name because two functions of the same name
/// could coexist in one module (overloading by parameter type). The new
/// grammar forbids redeclaration — a module's top-level names are unique
/// — so the name itself already disambiguates. The signature suffix is
/// gone.
///
/// No generic-instantiation form. The new grammar has no generics, so
/// there is no `generateMangledNameForGeneric`, no substitution walk,
/// no type-argument encoding. Every declaration is concrete.
///
/// No struct/enum overloads. The new grammar has neither. A table is
/// the only user-declarable type, and its mangled name follows the same
/// `_L<module>_<name>` shape as a non-exported function.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"

#include <string>

namespace lucid::sema {

struct SemaContext;   // forward declaration; defined in SemaContext.hpp

// ─────────────────────────────────────────────────────────────────────────────
// Per-declaration entry points
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Mangled name for a `FN` declaration.
///
/// An `@export`ed function's mangled name is `decl->name` (the source
/// name). A non-exported function's mangled name is
/// `_L<module-path>_<name>`.
///
/// The return value is always a valid `InternedString` — the empty
/// interned string is returned only if `decl` is null or the module's
/// path is empty, both of which are compiler bugs.
InternedString generateMangledName(FnDeclAST* decl, SemaContext& ctx);

/// @brief Mangled name for a top-level `let`/`const` declaration.
///
/// Same shape as a function's: exported → source name, non-exported →
/// `_L<module-path>_<name>`. A local `let` never reaches this function;
/// the `@export` attribute is rejected on a local by
/// `AttributeValidator`, and a non-exported local has no symbol to
/// publish.
InternedString generateMangledName(VarDeclAST* decl, SemaContext& ctx);

/// @brief Mangled name for a `TABLE` declaration.
///
/// Same shape as a function's. The table's mangled name is what the
/// bytecode module's table registry uses to reference the table across
/// modules, and what a future `.lucb` serializer writes into the
/// artifact.
InternedString generateMangledName(TableDeclAST* decl, SemaContext& ctx);

} // namespace lucid::sema