/**
 * @file compile/SlotAllocator.hpp
 *
 * @responsibility Assign a frame slot to every local binding and
 *                 parameter; track liveness across suspend points.
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
 * This class knows about frame slots and suspend-point liveness only.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/memory/InternedString.hpp"

namespace lucid::bytecode::compile {

class SlotAllocator {
public:
    SlotAllocator() = default;

    /// Allocate a slot for a parameter. Parameters get the lowest
    /// slots, in declaration order.
    uint16_t allocateParam(InternedString name);

    /// Allocate a slot for a local. Locals get slots after parameters.
    ///
    /// A local that shadows an outer binding gets a fresh slot; the
    /// map is updated to point at the new one. The outer binding's
    /// slot is not reclaimed (the function's frame is allocated for
    /// the whole function), but the outer binding is unreachable by
    /// name from this point on, which matches its scope.
    uint16_t allocateLocal(InternedString name);

    /// The slot for a name, or nullopt if the name has no slot in this
    /// function. A name with no slot is either a top-level binding or
    /// an error; the emitter distinguishes by the resolved declaration.
    std::optional<uint16_t> slotFor(InternedString name) const;

    /// The number of slots allocated (params + locals).
    uint32_t localSlotCount() const noexcept { return m_nextSlot; }

    // ─── Suspend-point liveness ─────────────────────────────────────────
    //
    // For a @sequence function, the emitter records which local slots
    // are live at each suspend point. At finalize time, the recorded
    // sets become ResumeEntry rows on the FunctionProto.

    /// Record the live-slot set at a suspend point. The resume index
    /// must be unique within the function. Every slot must be within
    /// the frame.
    void recordSuspendPoint(uint32_t resumeIndex,
                            std::vector<uint16_t> liveSlots);

    /// The recorded suspend points, in the order they were recorded
    /// (which is emission order, and therefore source order).
    const std::vector<std::pair<uint32_t, std::vector<uint16_t>>>&
        suspendPoints() const noexcept { return m_suspendPoints; }

private:
    std::unordered_map<InternedString, uint16_t> m_slots;
    uint32_t                                     m_nextSlot = 0;
    std::vector<std::pair<uint32_t, std::vector<uint16_t>>> m_suspendPoints;
};

} // namespace lucid::bytecode::compile