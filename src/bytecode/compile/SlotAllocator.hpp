/**
 * @file compile/SlotAllocator.hpp
 *
 * @responsibility Assign a frame slot to every local binding and
 *                 parameter; track liveness across suspend points;
 *                 produce localSlots and maxStackDepth for a
 *                 FunctionProto.
 *
 * ─── Naming note ──────────────────────────────────────────────────────────
 * The FileStructure calls this Frame.hpp/cpp. It was renamed to avoid
 * colliding with interp/Frame.hpp/cpp, which is the interpreter's
 * activation record. Two different concepts, one name; keeping them
 * distinct prevents a reader from thinking they share a representation.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/memory/InternedString.hpp"

namespace lucid::bytecode::compile {

class SlotAllocator {
public:
    SlotAllocator() = default;

    /// @brief Allocate a slot for a parameter. Parameters get the
    /// lowest slots, in declaration order.
    uint16_t allocateParam(InternedString name);

    /// @brief Allocate a slot for a local. Locals get slots after
    /// parameters.
    uint16_t allocateLocal(InternedString name);

    /// @brief The slot for a name, or nullopt if it has none.
    std::optional<uint16_t> slotFor(InternedString name) const;

    /// @brief The number of slots allocated (params + locals).
    uint32_t localSlotCount() const noexcept { return m_nextSlot; }

    // ─── Suspend-point liveness ─────────────────────────────────────────
    //
    // For a @sequence function, the caller records which slots are live
    // at each suspend point. At the end, the recorded sets become
    // ResumeEntry rows on the FunctionProto.

    /// @brief Record the live-slot set at a suspend point.
    void recordSuspendPoint(uint32_t resumeIndex,
                            std::vector<uint16_t> liveSlots);

    const std::vector<std::pair<uint32_t, std::vector<uint16_t>>>&
        suspendPoints() const noexcept { return m_suspendPoints; }

    // ─── Expression stack depth ─────────────────────────────────────────

    /// @brief Record the maximum expression-stack depth reached.
    void noteStackDepth(uint32_t depth);
    uint32_t maxStackDepth() const noexcept { return m_maxStackDepth; }

private:
    std::unordered_map<InternedString, uint16_t> m_slots;
    uint32_t                                     m_nextSlot = 0;
    uint32_t                                     m_maxStackDepth = 1;
    std::vector<std::pair<uint32_t, std::vector<uint16_t>>> m_suspendPoints;
};

} // namespace lucid::bytecode::compile