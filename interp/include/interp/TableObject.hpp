/**
 * @file interp/TableObject.hpp
 *
 * @responsibility The live representation of a Lucid table. A growing
 *                 table is a slot array with a free list and a
 *                 generation counter (grammar §4.1.1a); a fixed or
 *                 readonly table is a plain row array.
 *
 * ─── Design: one type, two shapes ─────────────────────────────────────────
 * A TableObject handles both growing and fixed tables. The `kind`
 * field distinguishes them; operations that are not valid for a kind
 * (ADD on a fixed table, for instance) are asserted in debug builds
 * and are prevented by the compiler in release builds.
 *
 * ─── Design: rows are slot-indexed, never moved ───────────────────────────
 * A row never moves between slots once added (§4.1.1a). REMOVE leaves
 * a dead slot; a later ADD reuses it with a new generation. A &T is a
 * {slot, generation} pair; reading a stale reference yields nil.
 *
 * ─── Design: the generation counter and reset floor ───────────────────────
 * Every growing table has a monotonically increasing generation
 * counter and a reset floor. A reference is valid when its generation
 * matches the slot's generation AND is not below the floor. CLEAR
 * raises the floor above every issued generation, invalidating every
 * reference into the table in O(1). Generation exhaustion panics
 * (Panic_GenerationExhausted, 7108) rather than wrapping.
 *
 * ─── Design: the primary index is a runtime choice ────────────────────────
 * A table with a @primary column has a `by<Column>` lookup (§4.1.5).
 * The index's representation is a runtime detail; the interpreter
 * promises only O(1) expected.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * runtime/Value.hpp. bytecode/TableSchema.hpp is forward-declared; the
 * .cpp includes it. contract/TypeDescriptor.hpp is forward-declared;
 * the .cpp includes it for the ResourcePlan dispatch in cell drops.
 */

#pragma once

#include "runtime/Value.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lucid::contract {
    struct TypeDescriptor;
}
namespace lucid::bytecode {
    struct TableSchema;
}

namespace lucid::interp {

using lucid::runtime::Value;

// ─────────────────────────────────────────────────────────────────────────────
// TableKind
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Which of the three table shapes this is.
///
/// Derived from the schema's isFixed/isReadonly/isPacked at
/// construction time and never changed. Grammar §4.1.1.
enum class TableKind : uint8_t {
    /// Growing: rows can be added and removed. Slot array + free list.
    Growing,

    /// Fixed: row set decided at declaration. Cells writable.
    /// Plain row array; no free list, no generations.
    Fixed,

    /// Readonly: row set decided at declaration, cells frozen.
    /// Plain row array; no free list, no generations.
    Readonly,
};

// ─────────────────────────────────────────────────────────────────────────────
// TableObject
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A live Lucid table.
///
/// Owned by the LoadedProgram (for a top-level table). A TableObject
/// is not refcounted; its lifetime is the LoadedProgram's.
class TableObject {
public:
    /// Construct a growing table with the given schema. The schema is
    /// borrowed, not owned; it must outlive the table.
    static std::unique_ptr<TableObject> makeGrowing(
        const bytecode::TableSchema* schema);

    /// Construct a fixed or readonly table with the given schema and
    /// initial rows. The schema is borrowed. The rows are moved in
    /// from the seed data (see StaticData).
    static std::unique_ptr<TableObject> makeFixed(
        const bytecode::TableSchema* schema,
        TableKind kind,
        std::vector<std::vector<Value>>&& seedRows);

    ~TableObject();

    TableObject(const TableObject&) = delete;
    TableObject& operator=(const TableObject&) = delete;

    // ─── Shape ──────────────────────────────────────────────────────────

    TableKind kind() const noexcept { return m_kind; }
    const bytecode::TableSchema* schema() const noexcept { return m_schema; }

    /// Number of columns.
    uint32_t columnCount() const noexcept;

    /// Column name by index. Precondition: index < columnCount().
    std::string_view columnName(uint32_t index) const noexcept;

    /// Column type by index. Precondition: index < columnCount().
    const contract::TypeDescriptor* columnType(uint32_t index) const noexcept;

    /// True if the column at `index` has @unique or @primary.
    bool columnIsUnique(uint32_t index) const noexcept;

    /// True if the column at `index` has @primary.
    bool columnIsPrimary(uint32_t index) const noexcept;

    /// True if the table has a @primary column.
    bool hasPrimary() const noexcept;

    /// The @primary column's index, or UINT32_MAX if none.
    uint32_t primaryColumn() const noexcept;

    // ─── Row set ────────────────────────────────────────────────────────

    /// Number of live rows.
    uint32_t count() const noexcept;

    /// Structural version: increments on every ADD, REMOVE, and CLEAR.
    /// Never decreases. Cell writes do not change it. Always 0 on a
    /// fixed/readonly table. Grammar §7.1.
    uint64_t version() const noexcept;

    // ─── Growing-table operations ───────────────────────────────────────

    /// Add a row. Precondition: kind == Growing,
    /// cells.size() == columnCount(). Runs @unique / @primary checks;
    /// a duplicate raises Panic_DuplicateKey (7107). Returns the new
    /// row as a RowRef Value.
    ///
    /// The cells are moved in (the caller's array is not reused).
    Value addRow(std::vector<Value>&& cells);

    /// Remove the row at slot `slot`. Precondition: kind == Growing,
    /// slot is live. Marks the slot dead, pushes it on the free list,
    /// removes its key from the @primary index if any. Every &T to
    /// the row becomes stale. O(1).
    void removeRow(uint32_t slot);

    /// Remove every row. Precondition: kind == Growing. Every &T into
    /// the table becomes stale in O(1); the @primary index is
    /// emptied; the reset floor is raised above every issued
    /// generation. Storage is retained. Grammar §4.1.1a, §4.1.1d.
    void clear();

    /// Best-effort release of unused trailing storage. Precondition:
    /// kind == Growing. Rows never move; no reference changes. The
    /// amount released is not guaranteed. Grammar §4.1.1d.
    void shrink();

    // ─── Row access ─────────────────────────────────────────────────────

    /// The RowRef Value for a live slot. Precondition: slot is live.
    Value rowRef(uint32_t slot) const noexcept;

    /// True if the given {slot, generation} is live and not below the
    /// reset floor. A false result means the reference is stale.
    bool isLiveRowRef(uint32_t slot, uint32_t generation) const noexcept;

    /// Read the value of column `col` in the row at `slot`.
    /// Precondition: slot is live, col < columnCount().
    const Value& cell(uint32_t slot, uint32_t col) const noexcept;

    /// Write the value of column `col` in the row at `slot`. Runs the
    /// @unique / @primary check if the column is unique; a duplicate
    /// raises Panic_DuplicateKey. Precondition: kind != Readonly,
    /// column is not @readonly, slot is live.
    void setCell(uint32_t slot, uint32_t col, Value v);

    // ─── Indexing ───────────────────────────────────────────────────────

    /// Look up a row by its @primary column's value. Returns a RowRef
    /// Value, or Nil if no match. Precondition: hasPrimary().
    ///
    /// O(1) expected. The index representation is a runtime choice;
    /// see §4.1.5.
    Value byPrimary(const Value& key) const noexcept;

    // ─── Iteration ──────────────────────────────────────────────────────

    /// Visit every live row in slot order, calling `fn(slot)` for each.
    /// Used by for-loop iteration.
    ///
    /// Iteration order is slot order (grammar §7.1).
    template <typename Fn>
    void forEachRow(Fn&& fn) const {
        for (uint32_t slot = 0; slot < m_capacity; ++slot) {
            if (isSlotLive(slot)) {
                fn(slot);
            }
        }
    }

private:
    TableObject(const bytecode::TableSchema* schema, TableKind kind);

    /// True if `slot` is within the capacity and live.
    bool isSlotLive(uint32_t slot) const noexcept;

    /// Internal: add a row without running uniqueness checks. Used by
    /// makeFixed for seed rows (Sema already checked them at compile
    /// time; see §4.1.5 "Duplicates in an initializer").
    void addRowUnchecked(std::vector<Value>&& cells);

    const bytecode::TableSchema* m_schema = nullptr;
    TableKind m_kind = TableKind::Growing;

    // Growing-only fields.
    uint32_t m_capacity = 0;
    uint32_t m_liveCount = 0;
    uint32_t m_freeListHead = UINT32_MAX;
    uint64_t m_generationCounter = 1;
    uint64_t m_resetFloor = 0;

    /// Row storage. For a growing table, one entry per slot (live or
    /// dead); dead entries are reused via the free list. For a fixed
    /// table, one entry per row. Flattened: m_cells[slot * columnCount + col].
    std::vector<Value> m_cells;

    /// Per-slot metadata (generation, liveness). Empty for a fixed
    /// table; for a growing table, one entry per slot.
    struct SlotMeta {
        uint32_t generation;
        bool     live;
    };
    std::vector<SlotMeta> m_slots;

    /// The @primary index, if the table has a @primary column. The
    /// concrete representation is hidden behind a forward declaration.
    struct PrimaryIndex;
    std::unique_ptr<PrimaryIndex> m_primary;

    /// The structural version. Increments on ADD, REMOVE, CLEAR.
    uint64_t m_version = 0;
};

} // namespace lucid::interp