/// @file GenericSubstitution.hpp
/// @brief Type substitution context for generic instantiation.
///
/// ───────────────────────────────────────────────────────────────────────────
/// 1.  PURPOSE
/// ───────────────────────────────────────────────────────────────────────────
///
/// `GenericSubstitution` is the core mechanism that bridges the gap between
/// **generic templates** (written with placeholders like `T`, `K`, `V`) and
/// **concrete instantiations** (generated with real types like `int`, `string`,
/// `Box<float>`).
///
/// It is a lightweight, read‑only context that maps:
///
///     Generic parameter name (e.g., "T")  →  Concrete type argument (e.g., int)
///
/// This mapping is used throughout CodeGen to:
///   - Lower AST types to LLVM types (substituting `T` with `int`).
///   - Generate unique symbol names for each instantiation.
///   - Detect and report arity mismatches clearly.
///
/// ───────────────────────────────────────────────────────────────────────────
/// 2.  WHY GENERIC SUBSTITUTION IS NEEDED
/// ───────────────────────────────────────────────────────────────────────────
///
/// Generic code is written once, but compiled many times — once for each set
/// of concrete type arguments. The compiler cannot treat `T` as a single,
/// fixed LLVM type because it changes per instantiation. `GenericSubstitution`
/// provides the per‑instantiation context that tells CodeGen what `T`
/// actually means at any given use site.
///
/// Without this structure:
///   - `getType()` would see the name `"T"` and try to look up an LLVM struct
///     named "T", which doesn't exist, creating bogus forward declarations.
///   - Mangling would produce the same symbol name for `identity<int>` and
///     `identity<float>`, causing symbol collisions at link time.
///   - Arity mismatches (e.g., `Box<T>` called with zero arguments) would
///     produce confusing "unknown type" errors instead of clear diagnostics.
///
/// ───────────────────────────────────────────────────────────────────────────
/// 3.  OWNERSHIP AND LIFETIME
/// ───────────────────────────────────────────────────────────────────────────
///
/// `GenericSubstitution` does NOT own any of the data it references. It holds
/// pointers/references to AST nodes that live in the arena (via the string
/// pool and the AST allocation arena). This means:
///
///   - It is cheap to copy (just two references + a trivial lookup loop).
///   - It is valid only as long as the underlying AST and type argument
///     vectors remain alive.
///   - It is intended to be constructed on the stack as a temporary context
///     during code generation, not stored persistently.
///
/// ───────────────────────────────────────────────────────────────────────────
/// 4.  USAGE SCENARIOS
/// ───────────────────────────────────────────────────────────────────────────
///
/// A. Type Lowering (`getType()`)
///    When CodeGen encounters a `NamedTypeAST` with name `"T"`, it calls
///    `subst.lookup("T")`. If the lookup returns `int`, `getType()` recursively
///    lowers that `int` primitive, and the generic parameter is replaced with
///    the concrete LLVM type.
///
///    This works at ANY depth — `Box<Vec<T>>` lowers to `Box<Vec<int>>` because
///    the substitution is passed recursively through `getType()`.
///
/// B. Name Mangling (`GenericMangledName.cpp`)
///    `typeToMangleString()` uses the substitution to replace generic
///    parameters with their concrete types BEFORE encoding the symbol name.
///    This ensures that `identity<int>` and `identity<float>` get distinct
///    mangled names.
///
/// C. Error Reporting (Arity Checking)
///    `isGenericParam()` allows the compiler to distinguish:
///      - "This name isn't a generic param at all" (fall through to normal
///        named‑type resolution, which may create a forward declaration).
///      - "This name IS a generic param, but no type argument was supplied"
///        (report a clear arity mismatch error, rather than silently creating
///        a bogus struct named "T").
///
/// ───────────────────────────────────────────────────────────────────────────
/// 5.  WHY THIS IS ITS OWN HEADER (NOT IN CODEGENGENERIC OR GENERICMANGLEDNAME)
/// ───────────────────────────────────────────────────────────────────────────
///
/// `GenericSubstitution` is needed in full (not just forward‑declared) by
/// both:
///   - `CodeGenGeneric.hpp/cpp`   (builds substitutions, drives instantiation)
///   - `GenericMangledName.hpp/cpp` (consumes substitutions to mangle names)
///
/// Putting the definition in either of those headers and having the other
/// include it would create a cycle:
///
///     CodeGenGeneric.hpp  ←─ includes ─→  GenericMangledName.hpp  ←─ includes ─→  CodeGenGeneric.hpp
///
/// A previous version worked around this with a forward declaration in
/// `GenericMangledName.hpp` and the real definition in `CodeGenGeneric.hpp`,
/// with `GenericMangledName.cpp` including `CodeGenGeneric.hpp` for the full
/// type. That avoided the cycle but made the ownership of the type unclear.
///
/// Giving `GenericSubstitution` its own header removes the ambiguity:
/// neither `CodeGenGeneric.hpp` nor `GenericMangledName.hpp` owns it; both
/// simply include it. This is the cleanest solution.
///
/// ───────────────────────────────────────────────────────────────────────────
/// 6.  KEY METHODS
/// ───────────────────────────────────────────────────────────────────────────
///
///   ┌───────────────────────────────────────────────────────────────────┐
///   │ TypeAST* lookup(InternedString name) const                        │
///   ├───────────────────────────────────────────────────────────────────┤
///   │ Returns the concrete type argument for the given generic          │
///   │ parameter name. Returns `nullptr` if:                             │
///   │   - `name` is not in `genericParams` (it's a user‑defined type).  │
///   │   - `name` IS in `genericParams` but `typeArgs` is missing an     │
///   │     entry (out‑of‑bounds).                                        │
///   │                                                                   │
///   │ Use `isGenericParam()` to distinguish these two failure modes.    │
///   └───────────────────────────────────────────────────────────────────┘
///
///   ┌───────────────────────────────────────────────────────────────────┐
///   │ bool isGenericParam(InternedString name) const                    │
///   ├───────────────────────────────────────────────────────────────────┤
///   │ Checks whether `name` is one of this substitution's generic       │
///   │ parameters, REGARDLESS of whether a type argument was supplied.   │
///   │                                                                   │
///   │ Distinguishes:                                                    │
///   │   - "This name isn't generic at all" → fall through to normal     │
///   │     named‑type resolution.                                        │
///   │   - "This name IS a generic parameter, but typeArgs is missing    │
///   │     an entry for it" → report an arity bug clearly, not silently  │
///   │     forward‑declare a struct named "T".                           │
///   └───────────────────────────────────────────────────────────────────┘
///
/// ───────────────────────────────────────────────────────────────────────────
/// 7.  EXAMPLE: BOX<T> INSTANTIATION
/// ───────────────────────────────────────────────────────────────────────────
///
/// Given:
///   struct Box<T> { value T }
///   let intBox = Box<int> { value = 42 }
///
/// The compiler builds a substitution:
///   genericParams = [GenericParamDeclAST("T")]
///   typeArgs      = [PrimitiveTypeAST(Int)]
///
/// When CodeGen processes the field `value T`:
///   1. `getType()` sees a `NamedTypeAST("T")` with `subst` non‑null.
///   2. Calls `subst.lookup("T")` → returns `PrimitiveTypeAST(Int)`.
///   3. Recursively calls `getType(ctx, PrimitiveTypeAST(Int), subst)`.
///   4. Returns `llvm::Type::getInt32Ty(...)`.
///
/// When mangling `Box<int>`:
///   1. `typeToMangleString()` sees `NamedTypeAST("T")` with `subst`.
///   2. `subst.lookup("T")` → `PrimitiveTypeAST(Int)`.
///   3. Encodes `"i"` (the mangling for `int`) instead of literal `"T"`.
///   4. Result: `"Box_i"` (distinct from `Box_f` for `Box<float>`).
///
/// ───────────────────────────────────────────────────────────────────────────
/// 8.  MEMORY AND PERFORMANCE
/// ───────────────────────────────────────────────────────────────────────────
///
/// The lookup operation is O(n) in the number of generic parameters, where
/// `n` is typically small (1–3 parameters). For larger parameter lists, the
/// compiler may choose to use a hash‑based lookup in the future, but the
/// current linear scan is sufficient for all realistic generic declarations.
///
/// Because the struct holds only references, it is cheap to pass by value
/// or by const reference, and constructing one is essentially free.
///
/// ───────────────────────────────────────────────────────────────────────────
/// 9.  FUTURE EXTENSIONS
/// ───────────────────────────────────────────────────────────────────────────
///
/// Future versions may add:
///   - Support for default type arguments (e.g., `Box<T = int>`).
///   - Support for dependent types (where one type argument depends on
///     another, e.g., `Array<T, size_of(T)>`).
///   - Caching of lookup results to speed up repeated substitutions.
///
/// Any such extension should preserve the existing API and the separation
/// of concerns between substitution, type lowering, and name mangling.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "core/memory/InternedString.hpp"

#include <vector>

namespace codegen {

/// @brief Context for substituting generic parameters with concrete types.
struct GenericSubstitution {
    const ArenaSpan<GenericParamDeclAST*>& genericParams;
    const std::vector<TypeAST*>& typeArgs;

    /// @brief Find the type argument for a given generic parameter name.
    /// @param name The generic parameter name.
    /// @return The substituted type, or nullptr if not found (either `name`
    ///         isn't a generic parameter at all, OR it is one but there's no
    ///         corresponding type argument - use isGenericParam() to tell
    ///         these two cases apart when that distinction matters).
    TypeAST* lookup(InternedString name) const {
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name && i < typeArgs.size()) {
                return typeArgs[i];
            }
        }
        return nullptr;
    }

    /// @brief Check whether `name` names one of this substitution's generic
    ///        parameters, regardless of whether a type argument was
    ///        actually supplied for it.
    ///
    /// Distinguishes "this name isn't generic at all" (fall through to
    /// normal named-type resolution) from "this name IS a generic
    /// parameter, but typeArgs is missing an entry for it" (an arity bug
    /// that should be reported clearly, not silently forwarded to
    /// getNamedType()'s forward-declaration fallback).
    bool isGenericParam(InternedString name) const {
        for (const auto* param : genericParams) {
            if (param->name == name) return true;
        }
        return false;
    }
};

} // namespace codegen