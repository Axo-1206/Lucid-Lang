/// @file ContextStack.cpp
/// @brief Implementation of ContextStack.

#include "ContextStack.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"

namespace sema {

// ─── ContextStack Implementation ───────────────────────────────────────────

void ContextStack::push(ContextKind kind, BaseAST* node, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = kind;
    frame.node = node;
    frame.openedAt = loc;
    m_stack.push_back(frame);
}

void ContextStack::pushFunction(FuncDeclAST* node, FuncTypeAST* funcType, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::FuncBody;
    frame.node = node;
    frame.openedAt = loc;
    frame.returnReqs = buildReturnRequirements(funcType);
    frame.returnReqs.currentLevel = 0;
    frame.returnReqs.currentGroupIndex = -1;  // No groups satisfied yet
    m_stack.push_back(frame);
}

void ContextStack::pushAnonFunction(AnonFuncExprAST* node, FuncTypeAST* funcType, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::FuncBody;
    frame.node = node;
    frame.openedAt = loc;
    frame.returnReqs = buildReturnRequirements(funcType);
    frame.returnReqs.currentLevel = 0;
    frame.returnReqs.currentGroupIndex = -1;
    m_stack.push_back(frame);
}

void ContextStack::pushLoop(StmtAST* loopStmt, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::LoopBody;
    frame.node = loopStmt;
    frame.openedAt = loc;
    frame.loopStmt = loopStmt;
    m_stack.push_back(frame);
}

void ContextStack::pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::SwitchBody;
    frame.node = switchStmt;
    frame.openedAt = loc;
    frame.switchStmt = switchStmt;
    m_stack.push_back(frame);
}

void ContextStack::pushBlock(BlockStmtAST* block, const SourceLocation& loc) {
    ContextFrame frame;
    frame.kind = ContextKind::Block;
    frame.node = block;
    frame.openedAt = loc;
    m_stack.push_back(frame);
}

void ContextStack::pop() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

// ─── Queries ─────────────────────────────────────────────────────────────

ContextKind ContextStack::current() const {
    return m_stack.empty() ? ContextKind::TopLevel : m_stack.back().kind;
}

BaseAST* ContextStack::currentNode() const {
    return m_stack.empty() ? nullptr : m_stack.back().node;
}

bool ContextStack::isInside(ContextKind kind) const {
    for (const auto& frame : m_stack) {
        if (frame.kind == kind) return true;
    }
    return false;
}

// ─── Return Requirements ──────────────────────────────────────────────────

const ReturnRequirements* ContextStack::currentReturnReqs() const {
    const ContextFrame* funcFrame = findInnermostFunction();
    return funcFrame ? &funcFrame->returnReqs : nullptr;
}

ReturnRequirements* ContextStack::currentReturnReqsMutable() {
    ContextFrame* funcFrame = findInnermostFunction();
    return funcFrame ? &funcFrame->returnReqs : nullptr;
}

bool ContextStack::hasReturnRequirements() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs && reqs->hasRequirements();
}

bool ContextStack::returnRequirementsSatisfied() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->isSatisfied() : true;
}

void ContextStack::advanceReturnGroup() {
    ReturnRequirements* reqs = currentReturnReqsMutable();
    if (reqs) {
        reqs->advanceGroup();
    }
}

const ReturnRequirements::Group* ContextStack::currentReturnGroup() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->currentGroup() : nullptr;
}

bool ContextStack::hasPendingRequirementAtCurrentLevel() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->hasPendingGroupAtCurrentLevel() : false;
}

void ContextStack::enterLevel() {
    ReturnRequirements* reqs = currentReturnReqsMutable();
    if (reqs) {
        reqs->enterLevel();
    }
}

void ContextStack::exitLevel() {
    ReturnRequirements* reqs = currentReturnReqsMutable();
    if (reqs) {
        reqs->exitLevel();
    }
}

int ContextStack::getCurrentLevel() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->getCurrentLevel() : 0;
}

// ─── If Condition Context ─────────────────────────────────────────────

void ContextStack::setIfConditionCtx(bool isIfCtx) {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->isIfConditionCtx = isIfCtx;
    }
}

bool ContextStack::isIfConditionCtx() const {
    const ContextFrame* frame = findInnermostIfContext();
    return frame ? frame->isIfConditionCtx : false;
}

void ContextStack::setHasElse(bool hasElse) {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->hasElse = hasElse;
    }
}

bool ContextStack::hasElse() const {
    const ContextFrame* frame = findInnermostIfContext();
    return frame ? frame->hasElse : false;
}

void ContextStack::setPendingNarrowing(const NarrowingInfo& info) {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->pendingNarrowing = info;
    }
}

const NarrowingInfo& ContextStack::getPendingNarrowing() const {
    static NarrowingInfo empty;
    const ContextFrame* frame = findInnermostIfContext();
    return frame ? frame->pendingNarrowing : empty;
}

void ContextStack::clearPendingNarrowing() {
    ContextFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->pendingNarrowing = NarrowingInfo();
    }
}

// ─── Type Narrowing ────────────────────────────────────────────────────

void ContextStack::pushNarrowingLevel(bool isInverse) {
    m_narrowing.pushLevel(isInverse);
}

void ContextStack::popNarrowingLevel() {
    m_narrowing.popLevel();
}

void ContextStack::narrowVariable(InternedString name, const TypeAST* type) {
    m_narrowing.narrow(name, type);
}

const TypeAST* ContextStack::getNarrowedType(InternedString name) const {
    return m_narrowing.getNarrowedType(name);
}

bool ContextStack::isNarrowingInverse() const {
    return m_narrowing.isInverse();
}

bool ContextStack::hasActiveNarrowing() const {
    return m_narrowing.hasNarrowing();
}

// ─── Pending Inverse Narrowing ──────────────────────────────────────

void ContextStack::setPendingInverseNarrowing(const NarrowingInfo& info) {
    ContextFrame* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = true;
        frame->pendingInverseNarrowing = info;
    }
}

bool ContextStack::hasPendingInverseNarrowing() const {
    const ContextFrame* frame = findInnermostBlock();
    return frame ? frame->hasPendingInverseNarrowing : false;
}

const NarrowingInfo& ContextStack::getPendingInverseNarrowing() const {
    static NarrowingInfo empty;
    const ContextFrame* frame = findInnermostBlock();
    return frame ? frame->pendingInverseNarrowing : empty;
}

void ContextStack::clearPendingInverseNarrowing() {
    ContextFrame* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = false;
        frame->pendingInverseNarrowing = NarrowingInfo();
    }
}

// ─── Convenience Queries ─────────────────────────────────────────────────

bool ContextStack::insideFunction() const {
    return isInside(ContextKind::FuncBody) ||
           isInside(ContextKind::AsyncBody);
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
        if (it->kind == ContextKind::FuncBody ||
            it->kind == ContextKind::AsyncBody) {
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

// ─── Helpers ─────────────────────────────────────────────────────────────

ReturnRequirements ContextStack::buildReturnRequirements(FuncTypeAST* funcType) {
    ReturnRequirements reqs;
    
    if (!funcType) return reqs;
    
    // Walk the function type chain
    // Each FuncTypeAST represents one parameter group
    // The returnType points to the next FuncTypeAST if curried, or a non-function type
    FuncTypeAST* current = funcType;
    int currentLevel = 0;
    bool sawArrow = false;  // Track if we've seen any arrow
    
    while (current) {
        ReturnRequirements::Group group;
        
        // A group requires a return if it has the arrow syntax
        group.requiresReturn = current->hasArrow;
        sawArrow = sawArrow || current->hasArrow;
        
        // Check if this group returns a function (currying)
        group.isCurried = current->returnType && current->returnType->isa<FuncTypeAST>();
        
        // The return type is either the next function type or the final type
        if (current->returnType) {
            if (group.isCurried) {
                // Return type is a function type
                group.returnType = current->returnType;
            } else {
                // Final return type (non-function)
                group.returnType = current->returnType;
            }
        } else {
            group.returnType = nullptr;  // Void return
        }
        
        // Assign level: each group with requiresReturn gets a new level
        // Groups without requiresReturn are at the same level as the next group
        if (group.requiresReturn) {
            group.level = currentLevel++;
        } else {
            group.level = currentLevel;
        }
        
        reqs.groups.push_back(group);
        
        // Move to the next function type if curried, otherwise stop
        if (group.isCurried) {
            current = static_cast<FuncTypeAST*>(current->returnType);
        } else {
            break;
        }
    }
    
    // Determine if void
    if (!reqs.groups.empty()) {
        const auto& lastGroup = reqs.groups.back();
        reqs.isVoid = !lastGroup.requiresReturn || (lastGroup.returnType == nullptr && !lastGroup.isCurried);
    } else {
        reqs.isVoid = true;
    }
    
    // allowsOptionalReturn: void functions or functions with no requirements
    reqs.allowsOptionalReturn = reqs.isVoid || !reqs.hasRequirements();
    
    return reqs;
}

ContextFrame* ContextStack::findInnermostFunction() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody ||
            it->kind == ContextKind::AsyncBody) {
            return &(*it);
        }
    }
    return nullptr;
}

const ContextFrame* ContextStack::findInnermostFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == ContextKind::FuncBody ||
            it->kind == ContextKind::AsyncBody) {
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