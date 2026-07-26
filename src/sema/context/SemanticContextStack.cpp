/// @file SemanticContextStack.cpp
/// @brief Implementation of SemanticContextStack.

#include "SemanticContextStack.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"

namespace sema {

// ─── TypeNarrowingStack Implementation ─────────────────────────────────────

void TypeNarrowingStack::pushLevel(bool isInverse) {
    NarrowingLevel level;
    level.isInverse = isInverse;
    m_stack.push_back(level);
}

void TypeNarrowingStack::popLevel() {
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

void TypeNarrowingStack::narrow(InternedString name, const TypeAST* type) {
    if (!m_stack.empty()) {
        m_stack.back().narrowedTypes[name] = type;
    }
}

const TypeAST* TypeNarrowingStack::getNarrowedType(InternedString name) const {
    // Search from innermost to outermost
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        auto found = it->narrowedTypes.find(name);
        if (found != it->narrowedTypes.end()) {
            return found->second;
        }
    }
    return nullptr;
}

bool TypeNarrowingStack::isInverse() const {
    return !m_stack.empty() && m_stack.back().isInverse;
}

bool TypeNarrowingStack::hasNarrowing() const {
    for (const auto& level : m_stack) {
        if (!level.narrowedTypes.empty()) {
            return true;
        }
    }
    return false;
}

void TypeNarrowingStack::clear() {
    m_stack.clear();
}

// ─── SemanticContextStack Implementation ──────────────────────────────────

SemanticContextStack::SemanticContextStack() = default;

void SemanticContextStack::push(SemanticContext kind, BaseAST* node, const SourceLocation& loc) {
    SemanticFrame frame;
    frame.kind = kind;
    frame.node = node;
    frame.openedAt = loc;
    m_stack.push_back(frame);
}

void SemanticContextStack::pushFunction(FuncDeclAST* node, FuncTypeAST* funcType, const SourceLocation& loc) {
    SemanticFrame frame;
    frame.kind = SemanticContext::FuncBody;
    frame.node = node;
    frame.openedAt = loc;
    frame.returnReqs = buildReturnRequirements(funcType);
    frame.returnReqs.currentLevel = 0;
    frame.returnReqs.currentGroupIndex = -1;  // No groups satisfied yet
    m_stack.push_back(frame);
}

void SemanticContextStack::pushAnonFunction(AnonFuncExprAST* node, FuncTypeAST* funcType, const SourceLocation& loc) {
    SemanticFrame frame;
    frame.kind = SemanticContext::FuncBody;
    frame.node = node;
    frame.openedAt = loc;
    frame.returnReqs = buildReturnRequirements(funcType);
    frame.returnReqs.currentLevel = 0;
    frame.returnReqs.currentGroupIndex = -1;
    m_stack.push_back(frame);
}

void SemanticContextStack::pushLoop(StmtAST* loopStmt, const SourceLocation& loc) {
    SemanticFrame frame;
    frame.kind = SemanticContext::LoopBody;
    frame.node = loopStmt;
    frame.openedAt = loc;
    frame.loopStmt = loopStmt;
    m_stack.push_back(frame);
}

void SemanticContextStack::pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc) {
    SemanticFrame frame;
    frame.kind = SemanticContext::SwitchBody;
    frame.node = switchStmt;
    frame.openedAt = loc;
    frame.switchStmt = switchStmt;
    m_stack.push_back(frame);
}

void SemanticContextStack::pushBlock(BlockStmtAST* block, const SourceLocation& loc) {
    SemanticFrame frame;
    frame.kind = SemanticContext::Block;
    frame.node = block;
    frame.openedAt = loc;
    m_stack.push_back(frame);
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

// ─── Return Requirements ──────────────────────────────────────────────────

const ReturnRequirements* SemanticContextStack::currentReturnReqs() const {
    const SemanticFrame* funcFrame = findInnermostFunction();
    return funcFrame ? &funcFrame->returnReqs : nullptr;
}

ReturnRequirements* SemanticContextStack::currentReturnReqsMutable() {
    SemanticFrame* funcFrame = findInnermostFunction();
    return funcFrame ? &funcFrame->returnReqs : nullptr;
}

bool SemanticContextStack::hasReturnRequirements() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs && reqs->hasRequirements();
}

bool SemanticContextStack::returnRequirementsSatisfied() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->isSatisfied() : true;
}

void SemanticContextStack::advanceReturnGroup() {
    ReturnRequirements* reqs = currentReturnReqsMutable();
    if (reqs) {
        reqs->advanceGroup();
    }
}

const ReturnRequirements::Group* SemanticContextStack::currentReturnGroup() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->currentGroup() : nullptr;
}

bool SemanticContextStack::hasPendingRequirementAtCurrentLevel() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->hasPendingGroupAtCurrentLevel() : false;
}

void SemanticContextStack::enterLevel() {
    ReturnRequirements* reqs = currentReturnReqsMutable();
    if (reqs) {
        reqs->enterLevel();
    }
}

void SemanticContextStack::exitLevel() {
    ReturnRequirements* reqs = currentReturnReqsMutable();
    if (reqs) {
        reqs->exitLevel();
    }
}

int SemanticContextStack::getCurrentLevel() const {
    const ReturnRequirements* reqs = currentReturnReqs();
    return reqs ? reqs->getCurrentLevel() : 0;
}

// ─── If Condition Context ─────────────────────────────────────────────

void SemanticContextStack::setIfConditionCtx(bool isIfCtx) {
    SemanticFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->isIfConditionCtx = isIfCtx;
    }
}

bool SemanticContextStack::isIfConditionCtx() const {
    const SemanticFrame* frame = findInnermostIfContext();
    return frame ? frame->isIfConditionCtx : false;
}

void SemanticContextStack::setHasElse(bool hasElse) {
    SemanticFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->hasElse = hasElse;
    }
}

bool SemanticContextStack::hasElse() const {
    const SemanticFrame* frame = findInnermostIfContext();
    return frame ? frame->hasElse : false;
}

void SemanticContextStack::setPendingNarrowing(const NarrowingInfo& info) {
    SemanticFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->pendingNarrowing = info;
    }
}

const NarrowingInfo& SemanticContextStack::getPendingNarrowing() const {
    static NarrowingInfo empty;
    const SemanticFrame* frame = findInnermostIfContext();
    return frame ? frame->pendingNarrowing : empty;
}

void SemanticContextStack::clearPendingNarrowing() {
    SemanticFrame* frame = findInnermostIfContext();
    if (frame) {
        frame->pendingNarrowing = NarrowingInfo();
    }
}

// ─── Type Narrowing ────────────────────────────────────────────────────

void SemanticContextStack::pushNarrowingLevel(bool isInverse) {
    m_narrowing.pushLevel(isInverse);
}

void SemanticContextStack::popNarrowingLevel() {
    m_narrowing.popLevel();
}

void SemanticContextStack::narrowVariable(InternedString name, const TypeAST* type) {
    m_narrowing.narrow(name, type);
}

const TypeAST* SemanticContextStack::getNarrowedType(InternedString name) const {
    return m_narrowing.getNarrowedType(name);
}

bool SemanticContextStack::isNarrowingInverse() const {
    return m_narrowing.isInverse();
}

bool SemanticContextStack::hasActiveNarrowing() const {
    return m_narrowing.hasNarrowing();
}

// ─── Pending Inverse Narrowing ──────────────────────────────────────

void SemanticContextStack::setPendingInverseNarrowing(const NarrowingInfo& info) {
    SemanticFrame* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = true;
        frame->pendingInverseNarrowing = info;
    }
}

bool SemanticContextStack::hasPendingInverseNarrowing() const {
    const SemanticFrame* frame = findInnermostBlock();
    return frame ? frame->hasPendingInverseNarrowing : false;
}

const NarrowingInfo& SemanticContextStack::getPendingInverseNarrowing() const {
    static NarrowingInfo empty;
    const SemanticFrame* frame = findInnermostBlock();
    return frame ? frame->pendingInverseNarrowing : empty;
}

void SemanticContextStack::clearPendingInverseNarrowing() {
    SemanticFrame* frame = findInnermostBlock();
    if (frame) {
        frame->hasPendingInverseNarrowing = false;
        frame->pendingInverseNarrowing = NarrowingInfo();
    }
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

BlockStmtAST* SemanticContextStack::currentBlock() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::Block) {
            return static_cast<BlockStmtAST*>(it->node);
        }
    }
    return nullptr;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

ReturnRequirements SemanticContextStack::buildReturnRequirements(FuncTypeAST* funcType) {
    ReturnRequirements reqs;
    
    // Collect all groups from the curry chain
    FuncTypeAST* current = funcType;
    int groupIndex = 0;
    int currentLevel = 0;
    
    while (current) {
        ReturnRequirements::Group group;
        group.requiresReturn = current->hasArrow;
        group.isCurried = current->isCurried();
        group.returnType = current->isCurried() ? current->returnTypes[0] : 
                          (current->returnTypes.empty() ? nullptr : current->returnTypes[0]);
        
        // Assign level: each group with requiresReturn gets a new level
        // Groups without requiresReturn are at the same level as the next group
        if (group.requiresReturn) {
            group.level = currentLevel++;
        } else {
            group.level = currentLevel;
        }
        
        reqs.groups.push_back(group);
        current = current->getNext();
        groupIndex++;
    }
    
    // Determine if void
    if (!reqs.groups.empty()) {
        const auto& lastGroup = reqs.groups.back();
        reqs.isVoid = !lastGroup.requiresReturn || (lastGroup.returnType == nullptr && !lastGroup.isCurried);
    } else {
        reqs.isVoid = true;
    }
    
    reqs.allowsOptionalReturn = reqs.isVoid || !reqs.hasRequirements();
    
    return reqs;
}

SemanticFrame* SemanticContextStack::findInnermostFunction() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::FuncBody ||
            it->kind == SemanticContext::AsyncBody ||
            it->kind == SemanticContext::GeneratorBody) {
            return &(*it);
        }
    }
    return nullptr;
}

const SemanticFrame* SemanticContextStack::findInnermostFunction() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::FuncBody ||
            it->kind == SemanticContext::AsyncBody ||
            it->kind == SemanticContext::GeneratorBody) {
            return &(*it);
        }
    }
    return nullptr;
}

SemanticFrame* SemanticContextStack::findInnermostIfContext() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::IfStmt) {
            return &(*it);
        }
    }
    return nullptr;
}

const SemanticFrame* SemanticContextStack::findInnermostIfContext() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::IfStmt) {
            return &(*it);
        }
    }
    return nullptr;
}

SemanticFrame* SemanticContextStack::findInnermostBlock() {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::Block) {
            return &(*it);
        }
    }
    return nullptr;
}

const SemanticFrame* SemanticContextStack::findInnermostBlock() const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->kind == SemanticContext::Block) {
            return &(*it);
        }
    }
    return nullptr;
}

} // namespace sema