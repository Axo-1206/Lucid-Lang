/**
 * @file DefiningTypeStack.cpp
 * @brief Implementation of DefiningTypeStack.
 *
 * Implements the stack of types currently being defined for self-reference
 * detection. This allows `checkRecursiveFieldType()` to distinguish:
 *   - `value Node<T>` → direct self-reference (illegal, infinite size)
 *   - `next ptr<Node<T>>?` → indirect (legal, breaks the cycle)
 */

#include "DefiningTypeStack.hpp"

namespace sema {

// ─── Push/Pop ────────────────────────────────────────────────────────

void DefiningTypeStack::beginDefining(TypeDeclAST* decl) {
    m_stack.push_back(decl);
}

void DefiningTypeStack::endDefining() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Queries ─────────────────────────────────────────────────────────

bool DefiningTypeStack::isDefining(TypeDeclAST* decl) const {
    for (TypeDeclAST* d : m_stack) {
        if (d == decl) return true;
    }
    return false;
}

TypeDeclAST* DefiningTypeStack::current() const {
    return m_stack.empty() ? nullptr : m_stack.back();
}

} // namespace sema