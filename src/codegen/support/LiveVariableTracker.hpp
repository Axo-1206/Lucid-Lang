/// @file support/LiveVariableTracker.hpp
/// @brief Tracks which variables are alive in the current scope for cleanup.
///
/// ─── Purpose ────────────────────────────────────────────────────────────────
/// When a block exits (via return, break, continue, or fall-through), we need
/// to clean up resources held by variables that are going out of scope.
/// This includes:
///   - Closure environments (__lucid_release_env)
///   - Heap-backed locals ([*]T, string)
///   - Scope exit callbacks (#scope_exit)
///
/// The LiveVariableTracker maintains a stack of scopes, each tracking which
/// variables are alive and which have been consumed. When a scope exits,
/// all alive variables are cleaned up.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"  // For BlockStmtAST

#include <unordered_set>
#include <vector>

namespace codegen {

/// @brief Tracks live variables in a single scope.
///
/// A scope is entered when a block, function, or loop body is entered.
/// Variables are marked alive when they are declared/initialized.
/// Variables are marked consumed when they are moved or go out of scope.
struct LiveVariableTracker {
    /// Variables that are currently alive in this scope.
    std::unordered_set<ValueDeclAST*> alive;

    /// Variables that have been consumed (moved, awaited, joined).
    std::unordered_set<ValueDeclAST*> consumed;

    /// The block this tracker belongs to, if any.
    /// Used to find #scope_exit registrations at cleanup time.
    /// Null for scopes not backed by a BlockStmtAST (e.g. loop-body wrappers).
    /// Set by CodeGenContext::pushLiveScope(block).
    /// @note This is only read at cleanup time - never mutated here.
    BlockStmtAST* block = nullptr;

    /// @brief Mark a variable as alive.
    /// @param decl The variable declaration.
    void markAlive(ValueDeclAST* decl) {
        if (decl) alive.insert(decl);
    }

    /// @brief Mark a variable as consumed.
    /// @param decl The variable declaration.
    void markConsumed(ValueDeclAST* decl) {
        if (!decl) return;
        alive.erase(decl);
        consumed.insert(decl);
    }

    /// @brief Check if a variable is alive.
    /// @param decl The variable declaration.
    /// @return True if the variable is alive, false otherwise.
    bool isAlive(ValueDeclAST* decl) const {
        return alive.find(decl) != alive.end();
    }

    /// @brief Check if a variable has been consumed.
    /// @param decl The variable declaration.
    /// @return True if the variable is consumed, false otherwise.
    bool isConsumed(ValueDeclAST* decl) const {
        return consumed.find(decl) != consumed.end();
    }

    /// @brief Get all alive variables.
    /// @return A vector of all alive declarations.
    std::vector<ValueDeclAST*> getAliveVariables() const {
        return std::vector<ValueDeclAST*>(alive.begin(), alive.end());
    }
};

} // namespace codegen