/// @file bytecode/compile/SlotAllocator.cpp
/// @brief Slot assignment, type tagging, and suspend-point liveness
///        for one function.

#include "SlotAllocator.hpp"

#include "contract/ResourcePlan.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

using namespace lucid::contract;

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Slot assignment
// ─────────────────────────────────────────────────────────────────────────────

uint16_t SlotAllocator::allocateParam(InternedString name,
                                       const TypeDescriptor& type) {
    AST_ASSERT_MSG(m_nextSlot < 0xFFFF,
        "SlotAllocator: ran out of parameter slots — a function with "
        "more than 65534 parameters is a compiler bug");

    auto it = m_slots.find(name);
    AST_ASSERT_MSG(it == m_slots.end(),
        "SlotAllocator: parameter was already allocated a slot");

    const uint16_t slot = static_cast<uint16_t>(m_nextSlot++);
    m_slots.emplace(name, slot);
    m_slotTypes.push_back(type);

    // Parameters are not recorded in any scope's dropSlots. Their
    // lifetime is the function's, not any block's, so the scope-exit
    // drop mechanism does not apply to them.
    //
    // KNOWN BUG: nothing drops parameters on return. The drop
    // scheduler (DropSchedule::emitReturnDrops) iterates the open
    // scopes' dropSlots, and a parameter is in none of them. A
    // parameter whose type owns a resource (a string, a host handle)
    // leaks its resource when the function returns. The fix is to
    // record parameter slots on the allocator and have
    // emitReturnDrops walk them in addition to the open scopes.
    return slot;
}

uint16_t SlotAllocator::allocateLocal(InternedString name,
                                       const TypeDescriptor& type) {
    AST_ASSERT_MSG(m_nextSlot < 0xFFFF,
        "SlotAllocator: ran out of local slots — a function with "
        "more than 65534 locals is a compiler bug");

    const uint16_t slot = static_cast<uint16_t>(m_nextSlot++);
    m_slots[name] = slot;
    m_slotTypes.push_back(type);

    // If a scope is open and the type's plan requires a drop at scope
    // exit, record the slot in the scope.
    if (!m_scopes.empty()) {
        const ResourcePlan plan =
            planForType(m_slotTypes[slot]);
        if (plan.needsDropForStorage()) {
            m_scopes.back().dropSlots.push_back(slot);
        }
    }

    return slot;
}

std::optional<uint16_t> SlotAllocator::slotFor(InternedString name) const {
    auto it = m_slots.find(name);
    if (it == m_slots.end()) return std::nullopt;
    return it->second;
}

const TypeDescriptor& SlotAllocator::typeOf(uint16_t slot) const {
    AST_ASSERT_MSG(slot < m_slotTypes.size(),
        "SlotAllocator::typeOf: slot index out of range — the caller "
        "asked for a slot that was never allocated");
    return m_slotTypes[slot];
}

// ─────────────────────────────────────────────────────────────────────────────
// Scopes
// ─────────────────────────────────────────────────────────────────────────────

void SlotAllocator::pushScope() {
    ScopeRecord scope;
    scope.startSlot = m_nextSlot;
    m_scopes.push_back(std::move(scope));
}

ScopeRecord SlotAllocator::popScope() {
    AST_ASSERT_MSG(!m_scopes.empty(),
        "SlotAllocator::popScope: no scope is open — the emitter "
        "popped a scope that was never pushed");
    ScopeRecord scope = std::move(m_scopes.back());
    m_scopes.pop_back();
    return scope;
}

const std::vector<uint16_t>&
SlotAllocator::currentScopeDropSlots() const {
    AST_ASSERT_MSG(!m_scopes.empty(),
        "SlotAllocator::currentScopeDropSlots: no scope is open — "
        "the caller queried a scope that was never pushed");
    return m_scopes.back().dropSlots;
}

std::vector<const std::vector<uint16_t>*>
SlotAllocator::openScopeDropSlots() const {
    std::vector<const std::vector<uint16_t>*> result;
    result.reserve(m_scopes.size());
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        result.push_back(&it->dropSlots);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Suspend-point liveness
// ─────────────────────────────────────────────────────────────────────────────

void SlotAllocator::recordSuspendPoint(uint32_t resumeIndex,
                                       std::vector<uint16_t> liveSlots) {
    for (const auto& entry : m_suspendPoints) {
        AST_ASSERT_MSG(entry.first != resumeIndex,
            "SlotAllocator: two suspend points share a resume index — "
            "the compiler allocated a resume index twice");
    }
    for (uint16_t slot : liveSlots) {
        AST_ASSERT_MSG(slot < m_nextSlot,
            "SlotAllocator: a suspend point's live set names a slot "
            "that has not been allocated");
    }
    m_suspendPoints.emplace_back(resumeIndex, std::move(liveSlots));
}

} // namespace lucid::bytecode::compile