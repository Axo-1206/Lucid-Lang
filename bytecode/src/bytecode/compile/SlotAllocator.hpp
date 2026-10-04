/**
 * @file compile/SlotAllocator.hpp
 *
 * @responsibility Assign a frame slot to every local binding and
 *                 parameter; record each slot's type; track liveness
 *                 across suspend points.
 *
 * ─── Naming note ──────────────────────────────────────────────────────────
 * The FileStructure calls this Frame.hpp/cpp. It was renamed to avoid
 * colliding with interp/Frame.hpp/cpp, which is the interpreter's
 * activation record.
 *
 * ─── What this class does NOT do ──────────────────────────────────────────
 * Stack-depth tracking is not this class's job. The value stack's
 * high-water mark is a property of the emitted code, not of the frame
 * slots, and it is tracked by CompilerContext (see noteStackEffect).
 *
 * ─── Types, not kinds ─────────────────────────────────────────────────────
 * Every slot is tagged with its full TypeDescriptor The type 
 * is what the drop scheduler consults: a `[string]` and a `string` 
 * are both OwnedBuffer, but their drops differ. 
 * The plan (from planForType) is a function of the type, so
 * the type is what the compiler keeps.
 *
 * ─── Scopes ───────────────────────────────────────────────────────────────
 * The emitter pushes a scope when it enters a block, and pops it when
 * it exits. The slots allocated between the marker and the pop belong
 * to that block. The scope records only the slots whose types need a
 * drop, so the drop scheduler iterates them directly.
 *
 * Popping a scope does NOT reclaim slots. The frame's size is fixed at
 * function entry (the interpreter allocates `localSlotCount()` slots
 * once per call). Scopes are a drop-emission concept, not a storage
 * concept.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "contract/TypeDescriptor.hpp"

#include "core/memory/InternedString.hpp"

namespace lucid::bytecode::compile {

/// @brief One scope's record: which slots were allocated in it.
struct ScopeRecord {
    /// The slot index at which this scope started (the value of
    /// m_nextSlot when the scope was pushed).
    uint32_t startSlot;

    /// The slots allocated in this scope whose types require a drop
    /// at scope exit, in allocation order. The drop scheduler emits
    /// drops for them in reverse order.
    std::vector<uint16_t> dropSlots;
};

class SlotAllocator {
public:
    SlotAllocator() = default;

    // ─── Slot assignment ────────────────────────────────────────────────

    /// Allocate a slot for a parameter.
    ///
    /// Parameters get the lowest slots, in declaration order. The
    /// parameter's type is recorded alongside the slot, and the slot
    /// is appended to paramSlots().
    uint16_t allocateParam(InternedString name, const contract::TypeDescriptor& type);

    /// The slots allocated to parameters, in declaration order.
    ///
    /// A return path drops them in reverse order, after every open
    /// scope's drops. Parameters live for the whole function; they
    /// are not in any scope's dropSlots, because a scope is a block
    /// and a parameter outlives every block. The drop scheduler
    /// reaches them through this accessor.
    ///
    /// Every parameter slot is in the list, regardless of the
    /// parameter's type. The drop scheduler skips the ones whose
    /// type's plan requires no drop; keeping the list
    /// type-agnostic means the producer side (allocateParam) does
    /// not need to consult the resource plan, and the consumer side
    /// (DropSchedule) already consults it for every slot it drops.
    const std::vector<uint16_t>& paramSlots() const noexcept {
        return m_paramSlots;
    }

    /// Allocate a slot for a local.
    ///
    /// Locals get slots after parameters, in allocation order. The
    /// local's type is recorded alongside the slot.
    ///
    /// If a scope is currently open and the type requires a drop at
    /// scope exit, the slot is also recorded in the scope's dropSlots
    /// list.
    uint16_t allocateLocal(InternedString name, const contract::TypeDescriptor& type);

    /// The slot for a name, or nullopt if the name has no slot in
    /// this function.
    std::optional<uint16_t> slotFor(InternedString name) const;

    /// The type of a slot. Precondition: slot < localSlotCount().
    const contract::TypeDescriptor& typeOf(uint16_t slot) const;

    /// The number of slots allocated (params + locals).
    uint32_t localSlotCount() const noexcept { return m_nextSlot; }

    // ─── Scopes ─────────────────────────────────────────────────────────

    /// Push a scope. Slots allocated after this belong to it until
    /// popScope is called.
    void pushScope();

    /// Pop the innermost scope and return it.
    ScopeRecord popScope();

    /// The innermost scope's drop slots, in allocation order.
    const std::vector<uint16_t>& currentScopeDropSlots() const;

    /// The drop slots of every open scope, innermost first.
    std::vector<const std::vector<uint16_t>*> openScopeDropSlots() const;

    /// The number of open scopes.
    size_t openScopeCount() const noexcept { return m_scopes.size(); }

    // ─── Suspend-point liveness ─────────────────────────────────────────

    void recordSuspendPoint(uint32_t resumeIndex,
                            std::vector<uint16_t> liveSlots);

    const std::vector<std::pair<uint32_t, std::vector<uint16_t>>>&
        suspendPoints() const noexcept { return m_suspendPoints; }

private:
    std::unordered_map<InternedString, uint16_t> m_slots;
    std::vector<contract::TypeDescriptor>        m_slotTypes;   // by slot index
    uint32_t                                     m_nextSlot = 0;

    /// The slots allocated to parameters, in declaration order.
    /// Parameters are not recorded in any scope's dropSlots — their
    /// lifetime is the function's, not any block's — so the drop
    /// scheduler reaches them through paramSlots(). See the
    /// accessor's doc for the drop order.
    std::vector<uint16_t> m_paramSlots;

    std::vector<ScopeRecord> m_scopes;

    std::vector<std::pair<uint32_t, std::vector<uint16_t>>> m_suspendPoints;
};

} // namespace lucid::bytecode::compile