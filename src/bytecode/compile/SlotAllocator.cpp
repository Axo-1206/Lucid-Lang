/// @file bytecode/compile/SlotAllocator.cpp
/// @brief Slot assignment and suspend-point liveness for one function.

#include "SlotAllocator.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Slot assignment
// ─────────────────────────────────────────────────────────────────────────────
//
// Parameters get slots first, in declaration order, starting at 0.
// Locals get slots after parameters, in the order allocateLocal is
// called (which is source order, because the emitters walk statements
// in order).
//
// The two-phase assignment (params first, then locals) is what lets
// the function's calling convention be "arguments arrive in the lowest
// slots." A caller that wants to place an argument in a specific slot
// knows where to put it; the interpreter knows where to read it.
//
// Slots are uint16_t. A function with more than 65535 locals is a
// compiler bug (or a pathological source file); the assert catches it.

uint16_t SlotAllocator::allocateParam(InternedString name) {
    AST_ASSERT_MSG(m_nextSlot < 0xFFFF,
        "SlotAllocator: ran out of parameter slots — a function with "
        "more than 65534 parameters is a compiler bug");

    // A parameter must not already have a slot. If it does, the
    // caller allocated it twice — a compiler bug.
    auto it = m_slots.find(name);
    AST_ASSERT_MSG(it == m_slots.end(),
        "SlotAllocator: parameter was already allocated a slot");

    const uint16_t slot = static_cast<uint16_t>(m_nextSlot++);
    m_slots.emplace(name, slot);
    return slot;
}

uint16_t SlotAllocator::allocateLocal(InternedString name) {
    AST_ASSERT_MSG(m_nextSlot < 0xFFFF,
        "SlotAllocator: ran out of local slots — a function with "
        "more than 65534 locals is a compiler bug");

    // A local may shadow a parameter or an earlier local in an outer
    // scope. The AST's scope resolution already decided which binding
    // a name refers to at each use site; the slot allocator's job is
    // to give each *declaration* a slot, not each name. So a name
    // that already has a slot (a shadowed outer binding) gets a new
    // slot for the inner declaration, and the map is updated to point
    // at the new one. The outer binding's slot is not lost; it is
    // simply no longer reachable through the name map after this
    // point, which is correct — the outer binding is out of scope.
    const uint16_t slot = static_cast<uint16_t>(m_nextSlot++);
    m_slots[name] = slot;
    return slot;
}

std::optional<uint16_t> SlotAllocator::slotFor(InternedString name) const {
    auto it = m_slots.find(name);
    if (it == m_slots.end()) return std::nullopt;
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Suspend-point liveness
// ─────────────────────────────────────────────────────────────────────────────
//
// The compiler records, at each suspend point, which local slots are
// live across the pause. These become the resume entries on the
// FunctionProto. A local that is not live across a suspend point does
// not need to survive the pause and is not recorded.
//
// The liveness analysis itself happens in the emitters (EmitStmt, when
// it lowers a suspend point). By the time recordSuspendPoint is called,
// the caller has already computed the live set. This class only stores
// it.

void SlotAllocator::recordSuspendPoint(uint32_t resumeIndex,
                                       std::vector<uint16_t> liveSlots) {
    // Resume indices are unique. Two entries with the same index would
    // make the resume table ambiguous.
    for (const auto& entry : m_suspendPoints) {
        AST_ASSERT_MSG(entry.first != resumeIndex,
            "SlotAllocator: two suspend points share a resume index — "
            "the compiler allocated a resume index twice");
    }

    // Every live slot is within the function's frame. A slot at or
    // above m_nextSlot means the caller recorded a slot that does not
    // exist yet — a compiler bug.
    for (uint16_t slot : liveSlots) {
        AST_ASSERT_MSG(slot < m_nextSlot,
            "SlotAllocator: a suspend point's live set names a slot "
            "that has not been allocated — the caller computed liveness "
            "against a different slot space than the one this allocator "
            "is producing");
    }

    m_suspendPoints.emplace_back(resumeIndex, std::move(liveSlots));
}

// ─────────────────────────────────────────────────────────────────────────────
// Stack depth
// ─────────────────────────────────────────────────────────────────────────────
//
// The emitters call noteStackDepth after every instruction whose
// effect on the expression stack could push the depth to a new high
// water mark. The maximum over the whole function becomes the
// FunctionProto's maxStackDepth.
//
// The method takes the new depth, not the delta. The emitter knows
// the depth after the instruction it just emitted; it does not need
// to compute a delta. This is simpler and less error-prone than
// tracking deltas.

void SlotAllocator::noteStackDepth(uint32_t depth) {
    if (depth > m_maxStackDepth) {
        m_maxStackDepth = depth;
    }
}

} // namespace lucid::bytecode::compile