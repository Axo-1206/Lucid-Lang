/**
 * @file SemanticContextStack.cpp
 * @brief Implementation of SemanticContextStack.
 *
 * Implements the semantic context stack that tracks what kind of
 * semantic construct we're currently inside (function body, loop body,
 * switch body, async body, etc.) for validation rules like:
 *   - Is `return` legal here?
 *   - Is `break` legal here?
 *   - Is `await` legal here?
 */

#include "SemanticContextStack.hpp"

namespace sema {

// ─── Push/Pop ────────────────────────────────────────────────────────────

void SemanticContextStack::push(SemanticContext kind, BaseAST* node, const SourceLocation& loc) {
    m_stack.push_back({kind, node, loc});
}

void SemanticContextStack::pop() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Queries ─────────────────────────────────────────────────────────────

SemanticContext SemanticContextStack::current() const {
    return m_stack.empty() ? SemanticContext::TopLevel : m_stack.back().kind;
}

BaseAST* SemanticContextStack::currentNode() const {
    return m_stack.empty() ? nullptr : m_stack.back().node;
}

bool SemanticContextStack::isInside(SemanticContext kind) const {
    for (const auto& frame : m_stack) {
        if (frame.kind == kind) return true;
    }
    return false;
}

// ─── Convenience Queries ─────────────────────────────────────────────────

bool SemanticContextStack::insideFunction() const {
    return isInside(SemanticContext::FuncBody) ||
           isInside(SemanticContext::AsyncBody) ||
           isInside(SemanticContext::GeneratorBody);
}

bool SemanticContextStack::insideLoop() const {
    return isInside(SemanticContext::LoopBody);
}

bool SemanticContextStack::insideSwitch() const {
    return isInside(SemanticContext::SwitchBody);
}

bool SemanticContextStack::insideAsync() const {
    return isInside(SemanticContext::AsyncBody);
}

bool SemanticContextStack::insideGenerator() const {
    return isInside(SemanticContext::GeneratorBody);
}

bool SemanticContextStack::insideParallel() const {
    return isInside(SemanticContext::ParallelBody);
}

FuncDeclAST* SemanticContextStack::currentFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::FuncBody ||
            it->kind == SemanticContext::AsyncBody ||
            it->kind == SemanticContext::GeneratorBody) {
            return static_cast<FuncDeclAST*>(it->node);
        }
    }
    return nullptr;
}

StmtAST* SemanticContextStack::currentLoop() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::LoopBody) {
            return static_cast<StmtAST*>(it->node);
        }
    }
    return nullptr;
}

SwitchStmtAST* SemanticContextStack::currentSwitch() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::SwitchBody) {
            return static_cast<SwitchStmtAST*>(it->node);
        }
    }
    return nullptr;
}

} // namespace sema