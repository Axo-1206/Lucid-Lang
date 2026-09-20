/// @file codegen/ownership/DropGlue.hpp
/// @brief Lazy generation of aggregate drop and copy functions.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// A struct containing a resource (a string field, a closure field, a
/// nested struct with resources) needs a `__drop_<type>` function that
/// walks its fields and drops each. It may also need a `__copy_<type>`
/// function that walks its fields and copies each (retaining closure
/// envs, deep-copying strings, recursively copying nested aggregates).
///
/// These functions are generated lazily, on first use, and cached in
/// `ProgramState`. Types with no resource fields get no glue — the cache
/// stores the "already determined to need no glue" fact so repeated
/// lookups are cheap.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the drop decision. `Ownership::drop` decides whether an
/// aggregate needs glue. This file only generates the glue once the
/// decision is made.
///
/// It is NOT user-code drop functions. Lucid doesn't have user-defined
/// destructors. The glue is generated, not referenced.

#pragma once

#include "codegen/Types.hpp"

#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace codegen {

class Ownership;

/// @brief Generate (or return cached) `__drop_<type>` for an aggregate type.
///
/// The generated function has signature `void(ptr %value)` where `%value`
/// points at the aggregate in memory. It walks the aggregate's fields in
/// order, calling `Ownership::drop` on each field that owns a resource.
///
/// Returns nullptr if the type is not an aggregate or if glue generation
/// fails (e.g. an unsupported field type).
///
/// Idempotent: the first call generates and caches; subsequent calls
/// return the cached function from `ProgramState::lookupDropGlue`.
llvm::Function* generateDropGlue(Ownership& ownership,
                                  TypeAST* type,
                                  ProgramState& program);

/// @brief Generate (or return cached) `__copy_<type>` for an aggregate type.
///
/// The generated function has signature `ptr(ptr %value)`. It allocates
/// a fresh aggregate (via `__lucid_alloc`), copies each field, and returns
/// a pointer to the new aggregate. Resource fields are copied according
/// to `Ownership::intoOwned`'s rules: closures are retained, strings are
/// deep-copied, nested aggregates are recursively copied.
///
/// Returns nullptr if the type is not an aggregate or if glue generation
/// fails.
///
/// Idempotent.
llvm::Function* generateCopyGlue(Ownership& ownership,
                                  TypeAST* type,
                                  ProgramState& program);

} // namespace codegen