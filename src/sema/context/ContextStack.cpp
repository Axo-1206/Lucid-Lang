/// @file ContextStack.cpp
/// @brief Implementation of ContextStack - semantic context management.
///
/// # Implementation Notes
///
/// ## Context Stack
///
/// The context stack tracks syntactic context for validation rules.
/// Each `push()` creates a frame, and `pop()` removes it. RAII guards
/// (`ScopedSemanticContext`) ensure proper cleanup.
///
/// ## Return Stack
///
/// The return stack is managed alongside the context stack. When a function
/// context is pushed, the expected return type is also pushed. When popped,
/// the return type is popped. This supports curried functions.
///
/// ## Narrowing Stack
///
/// The narrowing stack is separate from the context stack because narrowing
/// can persist across multiple contexts. For example, a narrowed type from
/// an if condition applies to the entire then branch, which may contain
/// nested blocks and loops.
///
/// ## Pending Inverse Narrowing
///
/// For standalone if statements with early exit (`if x == nil { return }`),
/// the inverse narrowing is stored on the innermost block context. When
/// `resolveBlock()` enters a new block, it checks for pending inverse
/// narrowing and applies it before resolving the block's statements.

#include "ContextStack.hpp"
#include "core/ast/TypeAST.hpp"

namespace sema {

// ─── Push/Pop ────────────────────────────────────────────────────────────

void ContextStack::push(ContextKind kind, BaseAST* node) {
    ContextFrame frame;
    frame.kind = kind;
    frame.node = node;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushFunction(FuncDeclAST* node, const TypeAST* returnType) {
    ContextFrame frame;
    frame.kind = ContextKind::FuncBody;
    frame.node = node;
    frame.expectedReturnType = returnType;
    m_stack.push_back(std::move(frame));
    m_returnStack.push(returnType);
}

void ContextStack::pushAnonFunction(AnonFuncExprAST* node, const TypeAST* returnType) {
    ContextFrame frame;
    frame.kind = ContextKind::FuncBody;
    frame.node = node;
    frame.expectedReturnType = returnType;
    m_stack.push_back(std::move(frame));
    m_returnStack.push(returnType);
}

void ContextStack::pushLoop(StmtAST* loopStmt) {
    ContextFrame frame;
    frame.kind = ContextKind::LoopBody;
    frame.node = loopStmt;
    frame.loopStmt = loopStmt;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushSwitch(SwitchStmtAST* switchStmt) {
    ContextFrame frame;
    frame.kind = ContextKind::SwitchBody;
    frame.node = switchStmt;
    frame.switchStmt = switchStmt;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushBlock(BlockStmtAST* block) {
    ContextFrame frame;
    frame.kind = ContextKind::Block;
    frame.node = block;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pop() {
    if (!m_stack.empty()) {
        ContextFrame& frame = m_stack.back();
        if (frame.kind == ContextKind::FuncBody) {
            m_returnStack.pop();
        }
        m_stack.pop_back();
    }
}

// ─── Context Queries ─────────────────────────────────────────────────────

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
    return isInside(ContextKind::FuncBody);
}

bool ContextStack::insideLoop() const {
    return isInside(ContextKind::LoopBody);
}

bool ContextStack::insideSwitch() const {
    return isInside(ContextKind::SwitchBody);
}

FuncDeclAST* ContextStack::currentFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody) {
            return static_cast<FuncDeclAST*>(it->node);
        }
    }
    return nullptr;
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

// ─── Type Narrowing ──────────────────────────────────────────────────────

bool ContextStack::isIfConditionCtx() const {
    auto* frame = findInnermostIfContext();
    return frame ? frame->isIfConditionCtx : false;
}

void ContextStack::setIfConditionCtx(bool isIfCtx) {
    auto* frame = findInnermostIfContext();
    if (frame) frame->isIfConditionCtx = isIfCtx;
}

void ContextStack::setHasElse(bool hasElse) {
    auto* frame = findInnermostIfContext();
    if (frame) frame->hasElse = hasElse;
}

bool ContextStack::hasElse() const {
    auto* frame = findInnermostIfContext();
    return frame ? frame->hasElse : false;
}

void ContextStack::setPendingNarrowing(const NarrowingInfo& info) {
    auto* frame = findInnermostIfContext();
    if (frame) frame->pendingNarrowing = info;
}

const NarrowingInfo& ContextStack::getPendingNarrowing() const {
    static NarrowingInfo empty;
    auto* frame = findInnermostIfContext();
    return frame ? frame->pendingNarrowing : empty;
}

void ContextStack::clearPendingNarrowing() {
    auto* frame = findInnermostIfContext();
    if (frame) frame->pendingNarrowing = NarrowingInfo{};
}

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

void ContextStack::narrowVariable(InternedString name, const TypeAST* type) {
    if (!m_narrowing.empty()) {
        m_narrowing.back().narrowedTypes[name] = type;
    }
}

const TypeAST* ContextStack::getNarrowedType(InternedString name) const {
    // Search from innermost to outermost
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

void ContextStack::setPendingInverseNarrowing(const NarrowingInfo& info) {
    auto* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = true;
        frame->pendingInverseNarrowing = info;
    }
}

bool ContextStack::hasPendingInverseNarrowing() const {
    auto* frame = findInnermostBlock();
    return frame ? frame->hasPendingInverseNarrowing : false;
}

const NarrowingInfo& ContextStack::getPendingInverseNarrowing() const {
    static NarrowingInfo empty;
    auto* frame = findInnermostBlock();
    return frame ? frame->pendingInverseNarrowing : empty;
}

void ContextStack::clearPendingInverseNarrowing() {
    auto* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = false;
        frame->pendingInverseNarrowing = NarrowingInfo{};
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────

ContextFrame* ContextStack::findInnermostFunction() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody) {
            return &(*it);
        }
    }
    return nullptr;
}

const ContextFrame* ContextStack::findInnermostFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody) {
            return &(*it);
        }
    }
    return nullptr;
}

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

} // namespace sema