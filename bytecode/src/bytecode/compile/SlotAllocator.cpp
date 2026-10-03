/// @file bytecode/compile/SlotAllocator.cpp
/// @brief Slot assignment and suspend-point liveness for one function.
///
/// ─── Scope ────────────────────────────────────────────────────────────────
/// This class does two things:
///
///   1. Assign a frame slot to every parameter and local.
///   2. Record the live-slot set at each suspend point of a @sequence
///      function.
///
/// Stack-depth tracking is NOT one of them. The value stack's
/// high-water mark is a property of the emitted code, not of the frame
/// slots, and it is tracked by CompilerContext (see its emitOpcode and
/// noteStackEffect). This separation keeps the two concerns distinct:
/// slots are about storage, depth is about the value stack.

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
// the function's calling convention be "argument i arrives in slot i."
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
    // to give each *declaration* a slot. A name that already has a
    // slot (a shadowed outer binding) gets a new slot for the inner
    // declaration; the map is updated to point at the new slot, so
    // the outer binding is unreachable by name from this point on.
    // That matches its scope.
    //
    // The outer binding's slot is not reclaimed. A function's frame
    // is sized for all its slots and allocated once, so the slot
    // stays reserved for the function's duration. Reclaiming it would
    // require a liveness analysis at the slot level, which is not
    // worth the complexity for the common case of one or two
    // shadowing locals.
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
// The emitter records, at each suspend point, which local slots are
// live across the pause. These become the resume entries on the
// FunctionProto. A local that is not live across a suspend point does
// not need to survive the pause and is not recorded.
//
// The liveness analysis itself happens in the emitter (EmitStmt, when
// it lowers a suspend point). By the time recordSuspendPoint is called,
// the caller has already computed the live set. This class only stores
// it.

void SlotAllocator::recordSuspendPoint(uint32_t resumeIndex,
                                       std::vector<uint16_t> liveSlots) {
    // Resume indices are unique within the function.
    for (const auto& entry : m_suspendPoints) {
        AST_ASSERT_MSG(entry.first != resumeIndex,
            "SlotAllocator: two suspend points share a resume index — "
            "the compiler allocated a resume index twice");
    }

    // Every live slot is within the function's frame.
    for (uint16_t slot : liveSlots) {
        AST_ASSERT_MSG(slot < m_nextSlot,
            "SlotAllocator: a suspend point's live set names a slot "
            "that has not been allocated — the caller computed "
            "liveness against a different slot space than the one "
            "this allocator is producing");
    }

    m_suspendPoints.emplace_back(resumeIndex, std::move(liveSlots));
}

} // namespace lucid::bytecode::compile