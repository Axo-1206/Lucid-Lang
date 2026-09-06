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
/// 3.  MEMORY MODEL
/// ───────────────────────────────────────────────────────────────────────────
///
/// `GenericSubstitution` uses `ArenaSpan` for both generic parameters and
/// type arguments. This matches the AST's memory model:
///   - Generic parameters are stored in `FuncDeclAST::genericParams` and
///     `StructDeclAST::genericParams` as `ArenaSpan<GenericParamDeclAST*>`.
///   - Type arguments are stored in AST nodes as `ArenaSpan<TypeAST*>`.
///   - No heap allocation is needed for substitution contexts.
///   - The substitution is trivially copyable (just two spans).
///
/// This is a significant improvement over using `std::vector`:
///   - No heap allocation at call sites
///   - No conversion overhead
///   - Consistent with the AST design
///   - Safe: the arena outlives all substitutions
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

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "core/memory/InternedString.hpp"

#include <vector>

namespace codegen {

/// @brief Context for substituting generic parameters with concrete types.
///
/// Uses ArenaSpan for both parameters and arguments, matching the AST's
/// memory model. No heap allocation is needed.
struct GenericSubstitution {
    /// The generic parameters from the declaration.
    const ArenaSpan<GenericParamDeclAST*>& genericParams;
    
    /// The concrete type arguments provided at the call/use site.
    const ArenaSpan<TypeAST*>& typeArgs;

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
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name) return true;
        }
        return false;
    }

    /// @brief Get the number of generic parameters.
    size_t paramCount() const {
        return genericParams.size();
    }

    /// @brief Get the number of type arguments provided.
    size_t argCount() const {
        return typeArgs.size();
    }

    /// @brief Check if the substitution has all required arguments.
    bool isComplete() const {
        return typeArgs.size() == genericParams.size();
    }

    /// @brief Get the generic parameter at a given index.
    GenericParamDeclAST* getParam(size_t index) const {
        if (index < genericParams.size()) {
            return genericParams[index];
        }
        return nullptr;
    }

    /// @brief Get the type argument at a given index.
    TypeAST* getArg(size_t index) const {
        if (index < typeArgs.size()) {
            return typeArgs[index];
        }
        return nullptr;
    }
};

} // namespace codegen