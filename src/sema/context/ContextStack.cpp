/// @file ContextStack.cpp
/// @brief Implementation of ContextStack.

#include "ContextStack.hpp"

namespace lucid::sema {

// ─── Push / pop ───────────────────────────────────────────────────────────

void ContextStack::push(ContextKind kind, BaseAST* node) {
    ContextFrame frame;
    frame.kind = kind;
    frame.node = node;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushFunction(FnDeclAST* decl, ContextKind kind,
                                TypeAST* returnType) {
    ContextFrame frame;
    frame.kind               = kind;
    frame.node               = decl;
    frame.expectedReturnType = returnType;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pop() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Context queries ──────────────────────────────────────────────────────

ContextKind ContextStack::current() const {
    return m_stack.empty() ? ContextKind::TopLevel : m_stack.back().kind;
}

bool ContextStack::isInside(ContextKind kind) const {
    for (const auto& frame : m_stack) {
        if (frame.kind == kind) return true;
    }
    return false;
}

BaseAST* ContextStack::currentNode() const {
    return m_stack.empty() ? nullptr : m_stack.back().node;
}

bool ContextStack::insideFunction() const {
    return isInside(ContextKind::FuncBody)
        || isInside(ContextKind::SequenceBody);
}

bool ContextStack::insideSequence() const {
    return isInside(ContextKind::SequenceBody);
}

bool ContextStack::insideLoop() const {
    return isInside(ContextKind::LoopBody);
}

bool ContextStack::insideSwitch() const {
    return isInside(ContextKind::SwitchBody);
}

StmtAST* ContextStack::currentLoop() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::LoopBody) {
            return static_cast<StmtAST*>(it->node);
        }
    }
    return nullptr;
}

SwitchStmtAST* ContextStack::currentSwitch() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::SwitchBody) {
            return static_cast<SwitchStmtAST*>(it->node);
        }
    }
    return nullptr;
}

BlockStmtAST* ContextStack::currentBlock() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::Block) {
            return static_cast<BlockStmtAST*>(it->node);
        }
    }
    return nullptr;
}

TypeAST* ContextStack::currentReturnType() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody
         || it->kind == ContextKind::SequenceBody) {
            return it->expectedReturnType;
        }
    }
    return nullptr;
}

// ─── If-condition context ─────────────────────────────────────────────────

bool ContextStack::isIfConditionCtx() const {
    const ContextFrame* frame = findInnermostIfContext();
    return frame ? frame->isIfConditionCtx : false;
}

void ContextStack::setIfConditionCtx(bool value) {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) frame->isIfConditionCtx = value;
}

void ContextStack::setHasElse(bool value) {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) frame->hasElse = value;
}

bool ContextStack::hasElse() const {
    const ContextFrame* frame = findInnermostIfContext();
    return frame ? frame->hasElse : false;
}

// ─── Pending narrowing ────────────────────────────────────────────────────

void ContextStack::setPendingNarrowing(const NarrowingInfo& info) {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) frame->pendingNarrowing = info;
}

const NarrowingInfo& ContextStack::getPendingNarrowing() const {
    static const NarrowingInfo empty;
    const ContextFrame* frame = findInnermostIfContext();
    return frame ? frame->pendingNarrowing : empty;
}

void ContextStack::clearPendingNarrowing() {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) frame->pendingNarrowing = NarrowingInfo{};
}

// ─── Narrowing stack ──────────────────────────────────────────────────────

void ContextStack::pushNarrowingLevel(bool isInverse) {
    NarrowingLevel level;
    level.isInverse = isInverse;
    m_narrowing.push_back(std::move(level));
}

void ContextStack::popNarrowingLevel() {
    if (!m_narrowing.empty()) {
        m_narrowing.pop_back();
    }
}

void ContextStack::narrowVariable(InternedString name, TypeAST* type) {
    if (!m_narrowing.empty()) {
        m_narrowing.back().narrowedTypes[name] = type;
    }
}

TypeAST* ContextStack::getNarrowedType(InternedString name) const {
    // Search from innermost outward. An inner narrowing level wins over an
    // outer one for the same name.
    for (auto it = m_narrowing.rbegin(); it != m_narrowing.rend(); ++it) {
        auto found = it->narrowedTypes.find(name);
        if (found != it->narrowedTypes.end()) {
            return found->second;
        }
    }
    return nullptr;
}

bool ContextStack::isNarrowingInverse() const {
    return !m_narrowing.empty() && m_narrowing.back().isInverse;
}

// ─── Pending inverse narrowing ────────────────────────────────────────────

void ContextStack::setPendingInverseNarrowing(const NarrowingInfo& info) {
    ContextFrame* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = true;
        frame->pendingInverseNarrowing    = info;
    }
}

bool ContextStack::hasPendingInverseNarrowing() const {
    const ContextFrame* frame = findInnermostBlock();
    return frame ? frame->hasPendingInverseNarrowing : false;
}

const NarrowingInfo& ContextStack::getPendingInverseNarrowing() const {
    static const NarrowingInfo empty;
    const ContextFrame* frame = findInnermostBlock();
    return frame ? frame->pendingInverseNarrowing : empty;
}

void ContextStack::clearPendingInverseNarrowing() {
    ContextFrame* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = false;
        frame->pendingInverseNarrowing    = NarrowingInfo{};
    }
}

// ─── Frame search helpers ─────────────────────────────────────────────────

ContextFrame* ContextStack::findInnermostIfContext() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::IfStmt) {
            return &(*it);
        }
    }
    return nullptr;
}

const ContextFrame* ContextStack::findInnermostIfContext() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::IfStmt) {
            return &(*it);
        }
    }
    return nullptr;
}

ContextFrame* ContextStack::findInnermostBlock() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::Block) {
            return &(*it);
        }
    }
    return nullptr;
}

const ContextFrame* ContextStack::findInnermostBlock() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::Block) {
            return &(*it);
        }
    }
    return nullptr;
}

} // namespace lucid::sema