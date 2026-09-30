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
 * calls; only `const` forbids it. So the values here are INITIAL values,
 * not the only values. The interpreter allocates live storage for each
 * one and copies the initial value in. The doc comment must say
 * "initial" everywhere to keep that clear.
 *
 * ─── Where the values come from ───────────────────────────────────────────
 * Sema folded every top-level let/const initializer and every baked row
 * of a @fixed/@readonly/growing-with-initializer table (grammar §3.4,
 * §4.1.1c). The compiler reads those folded values and copies them here
 * via BakeConstant. It does not evaluate anything.
 *
 * ─── What lives here ──────────────────────────────────────────────────────
 *   - every top-level let/const, keyed by mangled name
 *   - every @fixed/@readonly table's row set
 *   - every growing table's inline `= [ ... ]` initializer
 *
 * ─── What does NOT live here ──────────────────────────────────────────────
 *   - constants referenced by instructions (ConstantPool)
 *   - runtime state (the interpreter's live storage, allocated at load)
 */

#pragma once

#include "ConstantPool.hpp"   // for Constant
#include "TypeDescriptor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief One baked table.
struct BakedTable {
    std::string  mangledName;

    struct Column {
        std::string     mangledName;
        TypeDescriptor  type;
        bool            isUnique   = false;
        bool            isPrimary  = false;
        bool            isReadonly = false;
    };
    std::vector<Column> columns;

    /// The table's initial row set. Each row has exactly one Constant
    /// per column. These are INITIAL contents; a @fixed table's cells
    /// are writable after load, and a growing table may add rows.
    std::vector<std::vector<Constant>> rows;

    bool isFixed      = false;
    bool isReadonly   = false;
    bool isPacked     = false;
    bool isColumnar   = false;
    bool isRequest    = false;
    bool isHostBacked = false;

    /// Only meaningful if isHostBacked. The host symbol index for the
    /// table's type registration; -1 if not host-backed.
    int32_t hostTypeSymbolIndex = -1;

    std::optional<uint64_t> reservedCount;
};

/// @brief One baked top-level binding.
struct BakedBinding {
    std::string    mangledName;
    TypeDescriptor type;
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
    ///   - every row has exactly columns.size() cells.
    ///   - every @fixed/@readonly table has at least one row (§4.1.1b).
    ///   - every BakedBinding's initialValue.kind matches its type's shape.
    void checkInvariants() const;

private:
    std::vector<BakedTable>   m_tables;
    std::vector<BakedBinding> m_bindings;
};

} // namespace lucid::bytecode