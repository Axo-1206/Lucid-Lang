/**
 * @file SemanticResources.hpp
 * @brief Shared resources for the semantic phase.
 *
 * These are the immutable, session-wide resources that every semantic
 * pass needs access to. They are constructed once and shared across
 * all modules.
 *
 * @architectural_note Immutable by design
 *   None of these resources are modified during semantic analysis.
 *   The pool and arena are shared with the parser phase and only
 *   grow (allocation), never shrink or mutate existing data.
 */
#pragma once

#include "core/memory/ASTArena.hpp"
#include "core/memory/StringPool.hpp"

namespace sema {

/**
 * @brief Shared, immutable resources for the semantic phase.
 *
 * Holds the resources that are constructed once at the start of the
 * compilation session and never modified during semantic analysis.
 *
 * @note All members are references to externally-owned resources.
 *       The lifetime of these resources must exceed the lifetime of
 *       any SemanticResources instance.
 */
struct SemanticResources {
    /// String interner (shared with parser phase)
    StringPool& pool;

    /// AST allocator (shared with parser phase)
    ASTArena& arena;

    /**
     * @brief Construct with all required resources.
     *
     * @param p The string pool (must outlive this instance).
     * @param a The AST arena (must outlive this instance).
     */
    SemanticResources(StringPool& p, ASTArena& a)
        : pool(p), arena(a)
    {}

    // Non-copyable, non-movable (references can't be copied/moved)
    SemanticResources(const SemanticResources&) = delete;
    SemanticResources& operator=(const SemanticResources&) = delete;
    SemanticResources(SemanticResources&&) = delete;
    SemanticResources& operator=(SemanticResources&&) = delete;
};

} // namespace sema