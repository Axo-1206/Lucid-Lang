/// @file GenericSubstitution.hpp
/// @brief Type substitution context for generic instantiation.
///
/// ─── Why This Is Its Own File ────────────────────────────────────────────
/// GenericSubstitution is needed in full (not just forward-declared) by both:
///   - CodeGenGeneric.hpp/cpp    (builds substitutions, drives instantiation)
///   - GenericMangledName.hpp/cpp (consumes substitutions to mangle names,
///                                  substituting generic parameters at any
///                                  depth inside a type - see typeToMangleString)
///
/// Putting the definition in either of those headers and having the other
/// include it would create a cycle (CodeGenGeneric.hpp <-> GenericMangledName.hpp).
/// A previous version worked around this with a forward declaration in
/// GenericMangledName.hpp and the real definition in CodeGenGeneric.hpp, with
/// GenericMangledName.cpp including CodeGenGeneric.hpp for the full type -
/// that avoided the cycle but made GenericMangledName's ownership of the type
/// unclear. Giving GenericSubstitution its own header removes the ambiguity:
/// neither CodeGenGeneric.hpp nor GenericMangledName.hpp owns it, both just
/// include it.

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