/// @file NarrowingStack.hpp
/// @brief Tracks type narrowing per scope level.
/// 
/// When an if condition narrows a variable's type (e.g., `if x != nil`),
/// this stack tracks the narrowed type for each scope level.
/// 
/// The stack is searched from innermost to outermost when looking up a variable.
#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"

#include <unordered_map>
#include <vector>

namespace sema {

/// @brief Information about a type narrowing from an if condition.
/// 
/// Supports multiple variables (for `or` at top level):
///   if a == nil or b == nil { return }
///   -- inverse: a != nil AND b != nil
///   -- both a and b are narrowed
/// 
/// The `isEquality` flag distinguishes between `==` and `!=`:
///   - `==` (isEquality = true):  variable is nil/err in then branch
///   - `!=` (isEquality = false): variable is non-nullable/non-fallible in then branch
struct NarrowingInfo {
    bool hasNarrowing = false;
    std::unordered_map<InternedString, const TypeAST*> narrowings;  // varName → narrowed type
    bool isEquality = false;  // true for ==, false for !=
};

/// @brief One level of type narrowing for variables.
/// Each level corresponds to a branch (then, else) or inverse narrowing.
struct NarrowingLevel {
    std::unordered_map<InternedString, const TypeAST*> narrowedTypes;
    bool isInverse = false;  // Inverse narrowing applies (for rest of scope)
};

/// @brief Stack of type narrowing contexts.
/// 
/// When entering a branch (then/else), we push a level and apply narrowings.
/// When exiting the branch, we pop the level.
class NarrowingStack {
public:
    /// @brief Push a new narrowing level.
    /// @param isInverse True if this is inverse narrowing (applies to rest of scope).
    void pushLevel(bool isInverse = false);
    
    /// @brief Pop the current narrowing level.
    void popLevel();
    
    /// @brief Narrow a variable in the current level.
    /// @param name The variable name.
    /// @param type The narrowed type (e.g., int instead of int?).
    void narrow(InternedString name, const TypeAST* type);
    
    /// @brief Get the narrowed type for a variable.
    /// Searches from innermost to outermost. Returns nullptr if not narrowed.
    const TypeAST* getNarrowedType(InternedString name) const;
    
    /// @brief Check if current narrowing level is inverse.
    bool isInverse() const;
    
    /// @brief Check if there's any active narrowing.
    bool hasNarrowing() const;
    
    /// @brief Clear all narrowing levels (for testing/cleanup).
    void clear();
    
private:
    std::vector<NarrowingLevel> m_stack;
};

} // namespace sema