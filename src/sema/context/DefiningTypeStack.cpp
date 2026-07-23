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

void DefiningTypeStack::beginDefining(const TypeDeclAST* decl) {
    m_stack.push_back(decl);
}

void DefiningTypeStack::endDefining() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Queries ─────────────────────────────────────────────────────────

bool DefiningTypeStack::isDefining(const TypeDeclAST* decl) const {
    for (const TypeDeclAST* d : m_stack) {
        if (d == decl) return true;
    }
    return false;
}

const TypeDeclAST* DefiningTypeStack::current() const {
    return m_stack.empty() ? nullptr : m_stack.back();
}

} // namespace sema