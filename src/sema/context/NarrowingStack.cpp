/// @file NarrowingStack.cpp
/// @brief Implementation of NarrowingStack.

#include "NarrowingStack.hpp"

namespace sema {

// ─── Push/Pop ──────────────────────────────────────────────────────────────

void NarrowingStack::pushLevel(bool isInverse) {
    NarrowingLevel level;
    level.isInverse = isInverse;
    m_stack.push_back(level);
}

void NarrowingStack::popLevel() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Narrowing Operations ──────────────────────────────────────────────────

void NarrowingStack::narrow(InternedString name, const TypeAST* type) {
    if (!m_stack.empty()) {
        m_stack.back().narrowedTypes[name] = type;
    }
}

const TypeAST* NarrowingStack::getNarrowedType(InternedString name) const {
    // Search from innermost to outermost
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        auto found = it->narrowedTypes.find(name);
        if (found != it->narrowedTypes.end()) {
            return found->second;
        }
    }
    return nullptr;
}

// ─── Queries ────────────────────────────────────────────────────────────────

bool NarrowingStack::isInverse() const {
    return !m_stack.empty() && m_stack.back().isInverse;
}

bool NarrowingStack::hasNarrowing() const {
    for (const auto& level : m_stack) {
        if (!level.narrowedTypes.empty()) {
            return true;
        }
    }
    return false;
}

void NarrowingStack::clear() {
    m_stack.clear();
}

} // namespace sema