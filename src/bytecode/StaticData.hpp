/**
 * @file StaticData.hpp
 *
 * @responsibility Baked constant data: the initial contents of every
 *                 persistent declaration. Replaces the old design's
 *                 "module state" — in the new grammar there is no
 *                 module-load-time execution, only static data the
 *                 module starts with.
 *
 * ─── Design: everything here is a compile-time constant ───────────────────
 * Per §3.4 and §4.1.1c, a top-level let/const initializer and a
 * @fixed/@readonly table's inline rows are all const_expr. Sema folded
 * them; the compiler copies the folded values here.
 *
 * ─── Design: growing tables' initializers are also static data ────────────
 * A growing table's = [ ... ] initializer is a compile-time constant
 * block, seeded into the runtime's storage at startup. It lives here
 * alongside the fixed tables' rows.
 */

#pragma once

#include "ConstantPool.hpp"
#include "TypeDescriptor.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief One baked table.
struct BakedTable {
    std::string  mangledName;

    /// Column schema. One per column, in source order.
    struct Column {
        std::string     mangledName;
        TypeDescriptor  type;
        bool            isUnique   = false;
        bool            isPrimary  = false;
        bool            isReadonly = false;
    };
    std::vector<Column> columns;

    /// Baked rows. Each row has exactly one Constant per column.
    std::vector<std::vector<Constant>> rows;

    // Attribute flags (from §4.1.4)
    bool isFixed      = false;
    bool isReadonly   = false;
    bool isPacked     = false;
    bool isColumnar   = false;
    bool isRequest    = false;
    bool isHostBacked = false;

    /// Only meaningful if isHostBacked; the host symbol index for the
    /// table's type registration. -1 if not host-backed.
    int32_t hostTypeSymbolIndex = -1;

    /// Only meaningful if @reserve was written.
    std::optional<uint64_t> reservedCount;
};

/// @brief One baked top-level binding.
struct BakedBinding {
    std::string    mangledName;
    TypeDescriptor type;
    Constant       value;
};

/// @brief The whole set.
class StaticData {
public:
    StaticData() = default;

    std::vector<BakedTable>&        tables()   noexcept { return m_tables; }
    std::vector<BakedBinding>&      bindings() noexcept { return m_bindings; }
    const std::vector<BakedTable>&  tables()   const noexcept { return m_tables; }
    const std::vector<BakedBinding>& bindings() const noexcept { return m_bindings; }

    /// @brief Invariants. A violation is a compiler bug.
    ///   - every BakedTable has at least one column, unless it is
    ///     host-backed.
    ///   - every row has exactly columns.size() cells.
    ///   - every @fixed/@readonly table has at least one row (§4.1.1b).
    ///   - every BakedBinding's value.kind matches its type's shape.
    void checkInvariants() const;

private:
    std::vector<BakedTable>   m_tables;
    std::vector<BakedBinding> m_bindings;
};

} // namespace lucid::bytecode