/// @file ContextStack.cpp
/// @brief Implementation of simplified ContextStack.

#include "ContextStack.hpp"
#include "core/ast/TypeAST.hpp"

namespace sema {

// ─── Push/Pop ────────────────────────────────────────────────────────────

void ContextStack::push(ContextKind kind, BaseAST* node, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = kind;
    frame.node = node;
    frame.openedAt = loc;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushFunction(FuncDeclAST* node, FuncTypeAST* funcType, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::FuncBody;
    frame.node = node;
    frame.openedAt = loc;
    frame.returnReqs = buildReturnRequirements(funcType);
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushAnonFunction(AnonFuncExprAST* node, FuncTypeAST* funcType, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::FuncBody;
    frame.node = node;
    frame.openedAt = loc;
    frame.returnReqs = buildReturnRequirements(funcType);
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushLoop(StmtAST* loopStmt, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::LoopBody;
    frame.node = loopStmt;
    frame.openedAt = loc;
    frame.loopStmt = loopStmt;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::SwitchBody;
    frame.node = switchStmt;
    frame.openedAt = loc;
    frame.switchStmt = switchStmt;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pushBlock(BlockStmtAST* block, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::Block;
    frame.node = block;
    frame.openedAt = loc;
    m_stack.push_back(std::move(frame));
}

void ContextStack::pop() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Queries ──────────────────────────────────────────────────────────────

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
    return isInside(ContextKind::FuncBody) || isInside(ContextKind::AsyncBody);
}

bool ContextStack::insideLoop() const {
    return isInside(ContextKind::LoopBody);
}

bool ContextStack::insideSwitch() const {
    return isInside(ContextKind::SwitchBody);
}

bool ContextStack::insideAsync() const {
    return isInside(ContextKind::AsyncBody);
}

bool ContextStack::insideParallel() const {
    return isInside(ContextKind::ParallelBody);
}

FuncDeclAST* ContextStack::currentFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody || it->kind == ContextKind::AsyncBody) {
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

// ─── Return Requirements ─────────────────────────────────────────────────

bool ContextStack::hasReturnRequirements() const {
    auto* frame = findInnermostFunction();
    return frame && !frame->returnReqs.groups.empty();
}

bool ContextStack::returnRequirementsSatisfied() const {
    auto* frame = findInnermostFunction();
    if (!frame) return true;
    return frame->returnReqs.isSatisfied();
}

void ContextStack::advanceReturnGroup() {
    auto* frame = findInnermostFunction();
    if (!frame) return;
    frame->returnReqs.advanceGroup();
}

const ReturnRequirements::Group* ContextStack::currentReturnGroup() const {
    auto* frame = findInnermostFunction();
    if (!frame) return nullptr;
    return frame->returnReqs.currentGroup();
}

void ContextStack::enterLevel() {
    auto* frame = findInnermostFunction();
    if (frame) frame->returnReqs.enterLevel();
}

void ContextStack::exitLevel() {
    auto* frame = findInnermostFunction();
    if (frame) frame->returnReqs.exitLevel();
}

const ReturnRequirements* ContextStack::currentReturnReqs() const {
    auto* frame = findInnermostFunction();
    return frame ? &frame->returnReqs : nullptr;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

ReturnRequirements ContextStack::buildReturnRequirements(FuncTypeAST* funcType) {
    ReturnRequirements reqs;
    
    if (!funcType) return reqs;
    
    FuncTypeAST* current = funcType;
    int level = 0;
    
    while (current) {
        ReturnRequirements::Group group;
        group.requiresReturn = current->hasArrow;
        group.returnType = current->returnType;
        group.isCurried = current->returnType && current->returnType->isa<FuncTypeAST>();
        group.level = group.requiresReturn ? level++ : level;
        group.isSatisfied = false;
        reqs.groups.push_back(group);
        
        if (current->returnType && current->returnType->isa<FuncTypeAST>()) {
            current = static_cast<FuncTypeAST*>(current->returnType);
        } else {
            break;
        }
    }
    
    // Determine if void
    if (!reqs.groups.empty()) {
        const auto& lastGroup = reqs.groups.back();
        reqs.isVoid = !lastGroup.requiresReturn || 
                      (lastGroup.returnType == nullptr && !lastGroup.isCurried);
    } else {
        reqs.isVoid = true;
    }
    
    reqs.allowsOptionalReturn = reqs.isVoid || !reqs.hasRequirements();
    reqs.currentGroupIndex = -1;
    reqs.currentLevel = 0;
    
    return reqs;
}

ContextFrame* ContextStack::findInnermostFunction() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody || it->kind == ContextKind::AsyncBody) {
            return &(*it);
        }
    }
    return nullptr;
}

const ContextFrame* ContextStack::findInnermostFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody || it->kind == ContextKind::AsyncBody) {
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