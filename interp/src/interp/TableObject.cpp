/**
 * @file interp/TableObject.cpp
 *
 * @responsibility The live Lucid table implementation.
 *
 * ─── Design: two shapes, one class ────────────────────────────────────────
 * A TableObject is either growing (slot array + free list +
 * generations) or fixed (plain row array, no free list, no
 * generations). The `kind` field selects which. Every operation
 * checks the kind and asserts against misuse.
 *
 * ─── Design: row storage is row-major and flat ────────────────────────────
 * All cells live in one `m_cells` buffer, indexed as
 * `m_cells[slot * columnCount + col]`. This is a simple, cache-friendly
 * layout. A @columnar table (whose columns are stored separately) would
 * have a different layout, but the class exposes only `cell(slot, col)`
 * and `setCell(slot, col, v)`; the layout is hidden.
 *
 * ─── Design: drops go through planForType ─────────────────────────────────
 * When a cell is overwritten, removed, or cleared, the old value is
 * dropped. The classification is a pure function of the type;
 * contract::planForType provides it. The runtime's dropValue executes
 * it.
 *
 * ─── Design: the primary index is a pImpl ─────────────────────────────────
 * The @primary lookup is O(1) expected, but the representation is a
 * runtime choice. It is hidden behind a unique_ptr to a forward-
 * declared struct.
 *
 * ─── Design: failures throw runtime::RuntimeException ─────────────────────
 * Two operations can fail on a script-level rule: addRow and setCell,
 * both on a duplicate @unique or @primary value, and addRow
 * additionally on generation exhaustion. They throw the concrete
 * subclasses from runtime/Exceptions.hpp. Every other operation
 * documents a precondition that the interpreter checks before
 * calling; a violated precondition asserts in debug builds and is
 * undefined in release (the interpreter's check is the primary
 * defense).
 */

#include "interp/TableObject.hpp"

#include "bytecode/TableSchema.hpp"

#include "contract/ResourcePlan.hpp"
#include "contract/TypeDescriptor.hpp"

#include "runtime/Exceptions.hpp"
#include "runtime/String.hpp"
#include "runtime/Value.hpp"
#include "runtime/ValueOps.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace lucid::runtime;

namespace lucid::interp {

// ─────────────────────────────────────────────────────────────────────────
// PrimaryIndex
// ─────────────────────────────────────────────────────────────────────────

/// @brief The @primary column's lookup table.
///
/// Keys are the @primary column's values, encoded into a 64-bit hash
/// and then resolved against the stored row's actual cell value (to
/// handle collisions). Values are slot indices.
struct TableObject::PrimaryIndex {
    /// The hash of a value, using the type's natural hash.
    uint64_t hashValue(const Value& v,
                       const contract::TypeDescriptor& type) const noexcept;

    /// True if two values of the same type are equal, using the same
    /// equality rule as array CONTAINS.
    bool valuesEqual(const Value& a, const Value& b,
                     const contract::TypeDescriptor& type) const noexcept;

    std::unordered_multimap<uint64_t, uint32_t> entries;
};

// ─────────────────────────────────────────────────────────────────────────
// PrimaryIndex helpers
// ─────────────────────────────────────────────────────────────────────────

uint64_t
TableObject::PrimaryIndex::hashValue(
    const Value& v,
    const contract::TypeDescriptor& type) const noexcept
{
    using contract::TypeDescriptor;

    switch (type.kind) {
        case TypeDescriptor::Kind::Primitive: {
            switch (type.primitive) {
                case PrimitiveKind::String:
                    if (v.tag != ValueTag::String) return 0;
                    {
                        const runtime::StringObject* s = v.asString();
                        uint64_t h = 1469598103934665603ull;
                        for (uint32_t i = 0; i < s->length; ++i) {
                            h ^= static_cast<uint8_t>(s->data[i]);
                            h *= 1099511628211ull;
                        }
                        return h;
                    }
                case PrimitiveKind::Bool:
                case PrimitiveKind::Char:
                case PrimitiveKind::Int8:
                case PrimitiveKind::Int16:
                case PrimitiveKind::Int32:
                case PrimitiveKind::Int64:
                case PrimitiveKind::Uint8:
                case PrimitiveKind::Uint16:
                case PrimitiveKind::Uint32:
                case PrimitiveKind::Uint64:
                    return v.payload;
                default:
                    return v.payload;
            }
        }
        case TypeDescriptor::Kind::Named:
            // A host type. The registry's hash callback would be used
            // here; for v1, hash the payload pointer.
            return v.payload;
        default:
            return v.payload;
    }
}

bool
TableObject::PrimaryIndex::valuesEqual(
    const Value& a, const Value& b,
    const contract::TypeDescriptor& type) const noexcept
{
    using contract::TypeDescriptor;

    switch (type.kind) {
        case TypeDescriptor::Kind::Primitive:
            switch (type.primitive) {
                case PrimitiveKind::String:
                    if (a.tag != ValueTag::String ||
                        b.tag != ValueTag::String) return false;
                    return runtime::stringEquals(a.asString(), b.asString());
                default:
                    return a.tag == b.tag && a.payload == b.payload;
            }
        case TypeDescriptor::Kind::Named:
            return a.tag == b.tag && a.payload == b.payload;
        default:
            return a.tag == b.tag && a.payload == b.payload;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────

TableObject::TableObject(const bytecode::TableSchema* schema,
                         TableKind kind)
    : m_schema(schema)
    , m_kind(kind)
{
    assert(schema != nullptr);
}

TableObject::~TableObject() = default;

std::unique_ptr<TableObject>
TableObject::makeGrowing(const bytecode::TableSchema* schema) {
    auto t = std::unique_ptr<TableObject>(
        new TableObject(schema, TableKind::Growing));

    // @reserve(N): pre-size the cell buffer and slot array, and thread
    // every reserved slot onto the free list.
    if (schema->reservedCount) {
        const uint64_t reserve = *schema->reservedCount;
        const uint32_t cols = schema->columnCount();
        if (reserve > 0 && cols > 0 && reserve <= UINT32_MAX) {
            t->m_capacity = static_cast<uint32_t>(reserve);
            t->m_cells.resize(
                static_cast<size_t>(t->m_capacity) * cols);
            t->m_slots.resize(t->m_capacity);

            // Thread the slots onto the free list: slot i points at
            // slot i+1; the last points at UINT32_MAX.
            for (uint32_t i = 0; i < t->m_capacity; ++i) {
                t->m_slots[i].generation =
                    (i + 1 < t->m_capacity) ? (i + 1) : UINT32_MAX;
                t->m_slots[i].live = false;
            }
            t->m_freeListHead = 0;
        }
    }

    return t;
}

std::unique_ptr<TableObject>
TableObject::makeFixed(const bytecode::TableSchema* schema,
                       TableKind kind,
                       std::vector<std::vector<Value>>&& seedRows) {
    auto t = std::unique_ptr<TableObject>(
        new TableObject(schema, kind));

    const uint32_t cols = schema->columnCount();
    t->m_capacity = static_cast<uint32_t>(seedRows.size());
    t->m_cells.resize(static_cast<size_t>(t->m_capacity) * cols);

    for (uint32_t r = 0; r < seedRows.size(); ++r) {
        // Sema already checked the row's cell count and the
        // uniqueness of @primary/@unique values.
        auto& row = seedRows[r];
        for (uint32_t c = 0; c < cols; ++c) {
            t->m_cells[static_cast<size_t>(r) * cols + c] =
                std::move(row[c]);
        }
    }

    t->m_liveCount = static_cast<uint32_t>(seedRows.size());

    if (schema->hasPrimary()) {
        t->m_primary = std::make_unique<PrimaryIndex>();
        const uint32_t primary = schema->primaryColumnIndex();
        const contract::TypeDescriptor& keyType =
            schema->columns[primary].type;
        for (uint32_t r = 0; r < t->m_liveCount; ++r) {
            const Value& key =
                t->m_cells[static_cast<size_t>(r) * cols + primary];
            const uint64_t h = t->m_primary->hashValue(key, keyType);
            t->m_primary->entries.emplace(h, r);
        }
    }

    return t;
}

// ─────────────────────────────────────────────────────────────────────────
// Shape
// ─────────────────────────────────────────────────────────────────────────

uint32_t TableObject::columnCount() const noexcept {
    return m_schema ? m_schema->columnCount() : 0;
}

std::string_view TableObject::columnName(uint32_t index) const noexcept {
    if (!m_schema || index >= m_schema->columns.size()) return {};
    return m_schema->columns[index].mangledName;
}

const contract::TypeDescriptor*
TableObject::columnType(uint32_t index) const noexcept {
    if (!m_schema || index >= m_schema->columns.size()) return nullptr;
    return &m_schema->columns[index].type;
}

bool TableObject::columnIsUnique(uint32_t index) const noexcept {
    if (!m_schema || index >= m_schema->columns.size()) return false;
    const auto& col = m_schema->columns[index];
    return col.isUnique || col.isPrimary;
}

bool TableObject::columnIsPrimary(uint32_t index) const noexcept {
    if (!m_schema || index >= m_schema->columns.size()) return false;
    return m_schema->columns[index].isPrimary;
}

bool TableObject::hasPrimary() const noexcept {
    return m_schema && m_schema->hasPrimary();
}

uint32_t TableObject::primaryColumn() const noexcept {
    return m_schema ? m_schema->primaryColumnIndex() : UINT32_MAX;
}

// ─────────────────────────────────────────────────────────────────────────
// Row set
// ─────────────────────────────────────────────────────────────────────────

uint32_t TableObject::count() const noexcept {
    return m_liveCount;
}

uint64_t TableObject::version() const noexcept {
    if (m_kind != TableKind::Growing) return 0;
    return m_version;
}

// ─────────────────────────────────────────────────────────────────────────
// Slot liveness
// ─────────────────────────────────────────────────────────────────────────

bool TableObject::isSlotLive(uint32_t slot) const noexcept {
    if (slot >= m_capacity) return false;
    if (m_kind != TableKind::Growing) {
        return slot < m_liveCount;
    }
    if (slot >= m_slots.size()) return false;
    return m_slots[slot].live;
}

bool TableObject::isLiveRowRef(uint32_t slot,
                               uint32_t generation) const noexcept {
    if (!isSlotLive(slot)) return false;
    if (m_kind != TableKind::Growing) {
        return generation == 0;
    }
    if (slot >= m_slots.size()) return false;
    if (m_slots[slot].generation != generation) return false;
    if (m_slots[slot].generation < m_resetFloor) return false;
    return true;
}

Value TableObject::rowRef(uint32_t slot) const noexcept {
    if (m_kind != TableKind::Growing) {
        return Value::makeRowRef(slot, 0);
    }
    if (slot >= m_slots.size() || !m_slots[slot].live) {
        return Value::makeNil();
    }
    return Value::makeRowRef(slot, m_slots[slot].generation);
}

// ─────────────────────────────────────────────────────────────────────────
// Cell access
// ─────────────────────────────────────────────────────────────────────────

const Value& TableObject::cell(uint32_t slot, uint32_t col) const noexcept {
    // Precondition: slot is live, col < columnCount. The interpreter
    // checks both before calling.
    assert(isSlotLive(slot));
    assert(col < columnCount());

    const uint32_t cols = columnCount();
    return m_cells[static_cast<size_t>(slot) * cols + col];
}

// ─────────────────────────────────────────────────────────────────────────
// Growing-table operations
// ─────────────────────────────────────────────────────────────────────────

/// Look up a value in a table's @primary index and return the row's
/// slot, or UINT32_MAX if not found.
uint32_t TableObject::findPrimary(const TableObject::PrimaryIndex& idx,
                     const Value& key,
                     const contract::TypeDescriptor& keyType,
                     const std::vector<Value>& cells,
                     uint32_t cols,
                     uint32_t primaryCol) {
    const uint64_t h = idx.hashValue(key, keyType);
    auto range = idx.entries.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        const uint32_t slot = it->second;
        const Value& stored =
            cells[static_cast<size_t>(slot) * cols + primaryCol];
        if (idx.valuesEqual(stored, key, keyType)) return slot;
    }
    return UINT32_MAX;
}

Value TableObject::addRow(std::vector<Value>&& cells) {
    // Precondition: kind == Growing. The interpreter checks it.
    assert(m_kind == TableKind::Growing);
    assert(cells.size() == columnCount());

    const uint32_t cols = columnCount();

    // ─── Uniqueness checks ────────────────────────────────────────────
    //
    // For each @unique or @primary column, check that no live row
    // already holds this cell's value. A duplicate throws
    // DuplicateKeyError, which the interpreter catches and re-wraps
    // as a PanicException with code Panic_DuplicateKey.
    for (uint32_t c = 0; c < cols; ++c) {
        if (!m_schema->columns[c].isUnique &&
            !m_schema->columns[c].isPrimary) continue;

        const Value& candidate = cells[c];
        const contract::TypeDescriptor& colType = m_schema->columns[c].type;

        if (m_schema->columns[c].isPrimary && m_primary) {
            const uint32_t existing = findPrimary(
                *m_primary, candidate, colType,
                m_cells, cols, c);
            if (existing != UINT32_MAX) {
                throw runtime::DuplicateKeyError{};
            }
        } else {
            // A @unique but non-@primary column: linear scan.
            for (uint32_t s = 0; s < m_capacity; ++s) {
                if (!isSlotLive(s)) continue;
                const Value& stored =
                    m_cells[static_cast<size_t>(s) * cols + c];
                if (stored.tag == candidate.tag &&
                    stored.payload == candidate.payload) {
                    throw runtime::DuplicateKeyError{};
                }
            }
        }
    }

    // ─── Allocate a slot ──────────────────────────────────────────────
    uint32_t slot;

    if (m_freeListHead != UINT32_MAX) {
        slot = m_freeListHead;
        m_freeListHead = m_slots[slot].generation;
    } else {
        // Grow the slot array and the cell buffer.
        const uint32_t newCap = m_capacity == 0 ? 4 : m_capacity * 2;
        m_cells.resize(static_cast<size_t>(newCap) * cols);
        m_slots.resize(newCap);
        for (uint32_t i = m_capacity; i < newCap; ++i) {
            m_slots[i].generation = 0;
            m_slots[i].live = false;
        }
        slot = m_capacity;
        m_capacity = newCap;
    }

    // ─── Stamp a fresh generation ─────────────────────────────────────
    if (m_generationCounter == UINT64_MAX) {
        throw runtime::GenerationExhaustedError{};
    }
    const uint32_t gen = static_cast<uint32_t>(m_generationCounter++);

    if (slot >= m_slots.size()) {
        m_slots.resize(slot + 1);
    }
    m_slots[slot].generation = gen;
    m_slots[slot].live = true;

    // ─── Write the cells ──────────────────────────────────────────────
    for (uint32_t c = 0; c < cols; ++c) {
        m_cells[static_cast<size_t>(slot) * cols + c] = std::move(cells[c]);
    }

    ++m_liveCount;
    ++m_version;

    // ─── Update the primary index ─────────────────────────────────────
    const uint32_t primary = primaryColumn();
    if (primary != UINT32_MAX && m_primary) {
        const Value& key =
            m_cells[static_cast<size_t>(slot) * cols + primary];
        const uint64_t h =
            m_primary->hashValue(key, m_schema->columns[primary].type);
        m_primary->entries.emplace(h, slot);
    }

    return Value::makeRowRef(slot, gen);
}

void TableObject::removeRow(uint32_t slot) {
    // Precondition: kind == Growing, slot is live. The interpreter
    // checks both.
    assert(m_kind == TableKind::Growing);
    assert(isSlotLive(slot));

    const uint32_t cols = columnCount();

    // Drop every cell of the row.
    for (uint32_t c = 0; c < cols; ++c) {
        const contract::ResourcePlan plan =
            contract::planForType(m_schema->columns[c].type);
        if (plan.needsDropForStorage()) {
            runtime::dropValue(
                m_cells[static_cast<size_t>(slot) * cols + c], plan);
        }
    }

    // Remove the row's key from the primary index.
    const uint32_t primary = primaryColumn();
    if (primary != UINT32_MAX && m_primary) {
        const Value& key =
            m_cells[static_cast<size_t>(slot) * cols + primary];
        const uint64_t h =
            m_primary->hashValue(key, m_schema->columns[primary].type);
        auto range = m_primary->entries.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == slot) {
                m_primary->entries.erase(it);
                break;
            }
        }
    }

    // Mark the slot dead and push it onto the free list.
    m_slots[slot].live = false;
    m_slots[slot].generation = m_freeListHead;
    m_freeListHead = slot;

    --m_liveCount;
    ++m_version;
}

void TableObject::clear() {
    // Precondition: kind == Growing. The interpreter checks it.
    assert(m_kind == TableKind::Growing);

    const uint32_t cols = columnCount();

    // Drop every live cell.
    for (uint32_t s = 0; s < m_capacity; ++s) {
        if (!isSlotLive(s)) continue;
        for (uint32_t c = 0; c < cols; ++c) {
            const contract::ResourcePlan plan =
                contract::planForType(m_schema->columns[c].type);
            if (plan.needsDropForStorage()) {
                runtime::dropValue(
                    m_cells[static_cast<size_t>(s) * cols + c], plan);
            }
        }
    }

    // Empty the primary index.
    if (m_primary) {
        m_primary->entries.clear();
    }

    // Every slot becomes dead, and every existing reference becomes
    // stale. The reset floor is raised above every issued generation.
    m_resetFloor = m_generationCounter;

    // Rebuild the free list: all slots free, in ascending order.
    m_freeListHead = m_capacity > 0 ? 0 : UINT32_MAX;
    for (uint32_t i = 0; i < m_capacity; ++i) {
        m_slots[i].live = false;
        m_slots[i].generation = (i + 1 < m_capacity) ? (i + 1) : UINT32_MAX;
    }

    m_liveCount = 0;
    ++m_version;
}

void TableObject::shrink() {
    // Precondition: kind == Growing. The interpreter checks it.
    assert(m_kind == TableKind::Growing);

    // Find the highest live slot + 1; everything above it is dead.
    uint32_t newCap = m_capacity;
    while (newCap > 0 && !m_slots[newCap - 1].live) {
        --newCap;
    }

    if (newCap == m_capacity) return;

    // Rebuild the free list to exclude the trimmed slots.
    uint32_t newHead = UINT32_MAX;
    uint32_t newTail = UINT32_MAX;

    uint32_t cur = m_freeListHead;
    while (cur != UINT32_MAX) {
        const uint32_t next = m_slots[cur].generation;
        if (cur < newCap) {
            if (newHead == UINT32_MAX) newHead = cur;
            if (newTail != UINT32_MAX) {
                m_slots[newTail].generation = cur;
            }
            newTail = cur;
        }
        cur = next;
    }
    if (newTail != UINT32_MAX) {
        m_slots[newTail].generation = UINT32_MAX;
    }
    m_freeListHead = newHead;

    m_cells.resize(static_cast<size_t>(newCap) * columnCount());
    m_slots.resize(newCap);
    m_capacity = newCap;
    // SHRINK does not change the row set, so no version bump.
}

// ─────────────────────────────────────────────────────────────────────────
// Cell writes
// ─────────────────────────────────────────────────────────────────────────

void TableObject::setCell(uint32_t slot, uint32_t col, Value v) {
    // Preconditions: slot is live, col < columnCount, kind != Readonly,
    // column is not @readonly. The interpreter checks all four.
    assert(isSlotLive(slot));
    assert(col < columnCount());
    assert(m_kind != TableKind::Readonly);
    assert(!m_schema->columns[col].isReadonly);

    const uint32_t cols = columnCount();
    Value& oldCell = m_cells[static_cast<size_t>(slot) * cols + col];

    // If the new value equals the old (by tag and payload), this is a
    // no-op. The grammar says writing the value the cell already holds
    // is not a duplicate.
    if (oldCell.tag == v.tag && oldCell.payload == v.payload) {
        return;
    }

    // Duplicate check.
    if (m_schema->columns[col].isPrimary && m_primary) {
        const uint32_t existing = findPrimary(
            *m_primary, v, m_schema->columns[col].type,
            m_cells, cols, col);
        if (existing != UINT32_MAX && existing != slot) {
            throw runtime::DuplicateKeyError{};
        }
    } else if (m_schema->columns[col].isUnique) {
        for (uint32_t s = 0; s < m_capacity; ++s) {
            if (s == slot) continue;
            if (!isSlotLive(s)) continue;
            const Value& stored =
                m_cells[static_cast<size_t>(s) * cols + col];
            if (stored.tag == v.tag && stored.payload == v.payload) {
                throw runtime::DuplicateKeyError{};
            }
        }
    }

    // Update the primary index if the column is @primary.
    if (m_schema->columns[col].isPrimary && m_primary) {
        const uint64_t oldHash =
            m_primary->hashValue(oldCell, m_schema->columns[col].type);
        auto range = m_primary->entries.equal_range(oldHash);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == slot) {
                m_primary->entries.erase(it);
                break;
            }
        }
        const uint64_t newHash =
            m_primary->hashValue(v, m_schema->columns[col].type);
        m_primary->entries.emplace(newHash, slot);
    }

    // Drop the old value, then write the new one.
    const contract::ResourcePlan plan =
        contract::planForType(m_schema->columns[col].type);
    if (plan.needsDropForStorage()) {
        runtime::dropValue(oldCell, plan);
    }
    oldCell = v;
}

// ─────────────────────────────────────────────────────────────────────────
// Indexing
// ─────────────────────────────────────────────────────────────────────────

Value TableObject::byPrimary(const Value& key) const noexcept {
    const uint32_t primary = primaryColumn();
    if (primary == UINT32_MAX || !m_primary) {
        return Value::makeNil();
    }

    const uint32_t cols = columnCount();
    const uint32_t slot = findPrimary(
        *m_primary, key, m_schema->columns[primary].type,
        m_cells, cols, primary);

    if (slot == UINT32_MAX) return Value::makeNil();

    if (m_kind != TableKind::Growing) {
        return Value::makeRowRef(slot, 0);
    }
    return Value::makeRowRef(slot, m_slots[slot].generation);
}

} // namespace lucid::interp