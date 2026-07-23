/**
 * @file DefiningTypeStack.hpp
 * @brief Tracks types currently being defined for self-reference detection.
 *
 * Allows `checkRecursiveFieldType()` to distinguish:
 *   - `value Node<T>` → direct self-reference (illegal, infinite size)
 *   - `next ptr<Node<T>>?` → indirect (legal, breaks the cycle)
 *
 * @architectural_note Stack, not a single flag
 *   Definitions nest: a struct field whose type is an anonymous/local
 *   generic instantiation of another type still being defined is a real
 *   (if unusual) case worth being able to answer correctly at any depth.
 */
#pragma once

#include "core/ast/DeclAST.hpp"

#include <vector>

namespace sema {

/**
 * @brief Stack of types currently being defined.
 *
 * Pushed when a pass begins resolving a struct/enum/trait's own
 * internals (fields, variants), popped when it finishes.
 * 
 * @note Stores const pointers because AST nodes are read-only.
 */
class DefiningTypeStack {
public:
    // ─── Constructor ─────────────────────────────────────────────────────

    DefiningTypeStack() = default;

    // ─── Push/Pop ────────────────────────────────────────────────────────

    /**
     * @brief Mark a type declaration as "currently being defined".
     *
     * Prefer ScopedTypeDefinition over calling this directly.
     */
    void beginDefining(const TypeDeclAST* decl);

    /**
     * @brief Mark the current type as finished being defined.
     *
     * Prefer ScopedTypeDefinition's destructor over calling this directly.
     */
    void endDefining();

    // ─── Queries ─────────────────────────────────────────────────────────

    /**
     * @brief True if `decl` is somewhere on the currently-defining stack.
     *
     * This is the check that lets TypeChecker distinguish a field whose
     * type is the struct's own (still-being-defined) type from a field
     * whose type is some other, already-fully-analyzed type.
     */
    bool isDefining(const TypeDeclAST* decl) const;

    /**
     * @brief Get the innermost type currently being defined.
     * @return The innermost TypeDeclAST, or nullptr if none.
     */
    const TypeDeclAST* current() const;

    /**
     * @brief Get the stack (for saving/restoring).
     */
    const std::vector<const TypeDeclAST*>& stack() const { return m_stack; }

    /**
     * @brief Set the stack (for restoring).
     */
    void setStack(std::vector<const TypeDeclAST*> stack) { m_stack = std::move(stack); }

private:
    // ─── Members ─────────────────────────────────────────────────────────

    std::vector<const TypeDeclAST*> m_stack;
};

} // namespace sema