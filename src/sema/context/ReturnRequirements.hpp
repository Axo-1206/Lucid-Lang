/// @file ReturnRequirements.hpp
/// @brief Tracks return requirements for curried functions.
/// 
/// For a curried function like `(a int) -> (b int) -> int`, each `->` creates
/// a "return group" that the body must satisfy. The groups are tracked in order.
/// 
/// Example:
///   const add (a int) -> (int) -> int = {
///       // Group 0: (a int) -> (int) -> int  → requires returning a function
///       // Group 1: (b int) -> int           → requires returning an int
///       
///       return (b int) -> int {
///           return a + b
///       }
///   }
#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/SourceLocation.hpp"

#include <vector>

namespace sema {

/// @brief Return requirements for a curried function context.
/// 
/// Tracks each return group in order, including which level each group
/// belongs to (matching nesting depth) and whether it has been satisfied.
struct ReturnRequirements {
    /// @brief One return group in a curried function.
    /// 
    /// Each `->` in the function signature creates one group that must be
    /// satisfied by a `return` statement at the correct nesting level.
    struct Group {
        const TypeAST* returnType = nullptr;   // Expected return type (nullptr for void)
        bool isCurried = false;                // True if must return a function
        bool requiresReturn = false;           // True if this group requires a return
        bool isSatisfied = false;              // Has this group been satisfied?
        int level = 0;                         // Which nesting level this group belongs to
        SourceLocation satisfiedAt;            // Where it was satisfied (for diagnostics)
    };
    
    std::vector<Group> groups;                 // All return groups in order
    int currentGroupIndex = -1;                // Currently active group (-1 = none)
    int currentLevel = 0;                      // Current nesting level
    bool isVoid = false;                       // No return required (codegen wraps)
    bool allowsOptionalReturn = false;         // `return;` is allowed
    
    /// @brief Enter a new nesting level (called when entering a block).
    void enterLevel() { currentLevel++; }
    
    /// @brief Exit the current nesting level (called when exiting a block).
    void exitLevel() { if (currentLevel > 0) currentLevel--; }
    
    /// @brief Get the current nesting level.
    int getCurrentLevel() const { return currentLevel; }
    
    /// @brief Check if there are any pending groups at the current level.
    bool hasPendingGroupAtCurrentLevel() const {
        for (int i = currentGroupIndex + 1; i < static_cast<int>(groups.size()); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel) {
                return true;
            }
        }
        return false;
    }
    
    /// @brief Get the next pending group at the current level.
    const Group* getNextPendingGroupAtCurrentLevel() const {
        for (int i = currentGroupIndex + 1; i < static_cast<int>(groups.size()); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                return &groups[i];
            }
        }
        return nullptr;
    }
    
    /// @brief Get the current group (first unsatisfied group at current level).
    const Group* currentGroup() const {
        for (int i = currentGroupIndex + 1; i < static_cast<int>(groups.size()); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                return &groups[i];
            }
        }
        return nullptr;
    }
    
    /// @brief Mark the current group as satisfied and advance.
    void advanceGroup() {
        for (int i = currentGroupIndex + 1; i < static_cast<int>(groups.size()); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                groups[i].isSatisfied = true;
                currentGroupIndex = i;
                return;
            }
        }
    }
    
    /// @brief Check if all requirements are satisfied.
    bool isSatisfied() const {
        for (const auto& g : groups) {
            if (g.requiresReturn && !g.isSatisfied) return false;
        }
        return true;
    }
    
    /// @brief Check if there are any requirements.
    bool hasRequirements() const {
        for (const auto& g : groups) {
            if (g.requiresReturn) return true;
        }
        return false;
    }
};

} // namespace sema