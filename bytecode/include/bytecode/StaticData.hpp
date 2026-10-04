/**
 * @file StaticData.hpp
 *
 * @responsibility The initial state of every persistent declaration.
 *                 StaticData is a SEED, not storage: the interpreter
 *                 reads it once at load time, allocates mutable slots
 *                 for each binding and each table cell, and copies
 *                 these values in. After load, StaticData is not
 *                 consulted again.
 *
 * ─── Why "initial", not "constant" ────────────────────────────────────────
 * A @fixed table's cells are writable; only @readonly freezes them. A
 * top-level `let` may be reassigned by an exported function the host
 * calls; only `const` forbids it. So the values here are INITIAL
 * values, not the only values. The interpreter allocates live storage
 * for each one and copies the initial value in.
 *
 * ─── Where the values come from ───────────────────────────────────────────
 * Sema folded every top-level let/const initializer and every baked
 * row of a @fixed/@readonly/growing-with-initializer table (grammar
 * §3.4, §4.1.1c). The compiler reads those folded values and copies
 * them here via BakeConstant. It does not evaluate anything.
 *
 * ─── What lives here ──────────────────────────────────────────────────────
 *   - every top-level let/const, keyed by mangled name
 *   - every @fixed/@readonly table's row set
 *   - every growing table's inline `= [ ... ]` initializer
 *
 * ─── What does NOT live here ──────────────────────────────────────────────
 *   - constants referenced by instructions (ConstantPool)
 *   - runtime state (the interpreter's live storage, allocated at load)
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * contract/TypeDescriptor.hpp (for BakedBinding::type),
 * bytecode/ConstantPool.hpp (for Constant),
 * bytecode/TableSchema.hpp (for BakedTable::schema).
 */

#pragma once

#include "bytecode/ConstantPool.hpp"     // for Constant
#include "bytecode/TableSchema.hpp"      // for TableSchema
#include "contract/TypeDescriptor.hpp"   // for TypeDescriptor

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief One baked table: a schema plus its seed rows.
///
/// The schema is the table's shape (columns, types, attributes); the
/// rows are its initial contents. The two are separated because the
/// interpreter's TableObject needs the schema but not the seed rows
/// (it copies the rows into its own live storage at load time, then
/// never looks at BakedTable::rows again).
struct BakedTable {
    /// The table's mangled name. Tooling uses it for per-module views;
    /// the interpreter's runtime does not.
    std::string mangledName;

    /// The table's shape. See TableSchema.hpp.
    TableSchema schema;

    /// The table's initial row set. Each row has exactly one Constant
    /// per column. These are INITIAL contents; a @fixed table's cells
    /// are writable after load, and a growing table may add rows.
    ///
    /// For a host-backed table, this is empty: the host's type
    /// registration defines the shape, and the table has no rows of
    /// its own.
    std::vector<std::vector<Constant>> rows;
};

/// @brief One baked top-level binding.
struct BakedBinding {
    std::string    mangledName;
    contract::TypeDescriptor type;
    Constant       initialValue;   ///< INITIAL, not final
};

/// @brief The whole set.
class StaticData {
public:
    StaticData() = default;

    std::vector<BakedTable>&         tables()   noexcept { return m_tables; }
    std::vector<BakedBinding>&       bindings() noexcept { return m_bindings; }
    const std::vector<BakedTable>&   tables()   const noexcept { return m_tables; }
    const std::vector<BakedBinding>& bindings() const noexcept { return m_bindings; }

    /// @brief Invariants. A violation is a compiler bug.
    ///   - every non-host-backed BakedTable has at least one column.
    ///   - every row has exactly schema.columnCount() cells.
    ///   - every @fixed/@readonly table has at least one row (§4.1.1b).
    ///   - every BakedBinding's initialValue.kind matches its type's shape.
    ///   - at most one column per table has isPrimary.
    ///   - a host-backed table has no columns and no rows.
    ///   - @packed implies @fixed or @readonly.
    ///   - @fixed and @readonly are never both set.
    ///   - @reserve is only on a growing table.
    void checkInvariants() const;

private:
    std::vector<BakedTable>   m_tables;
    std::vector<BakedBinding> m_bindings;
};

} // namespace lucid::bytecode