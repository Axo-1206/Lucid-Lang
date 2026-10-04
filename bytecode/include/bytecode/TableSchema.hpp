/**
 * @file bytecode/TableSchema.hpp
 *
 * @responsibility The shape of one table: its columns, their types,
 *                 their per-column attributes, and the table-level
 *                 attributes. Extracted from BakedTable so the concept
 *                 has a name and so TableObject can depend on the
 *                 shape without depending on the seed rows.
 *
 * ─── Design: schema vs. data ──────────────────────────────────────────────
 * A TableSchema is the *description* of a table: "this table has a
 * @primary int column called id and a string column called name."
 * A BakedTable is a schema plus its seed rows. A TableObject is a
 * schema plus its live rows and its runtime state (free list,
 * generations, @primary index).
 *
 * ─── Design: why this is not in contract/ ─────────────────────────────────
 * The contract library holds the *vocabulary* both sides agree on:
 * the instruction set, the runtime operation set, the type shape, the
 * resource classification. A TableSchema is not part of that
 * vocabulary; it's the shape of a specific declaration's data, baked
 * into the artifact alongside the rows. It is artifact payload, like
 * FunctionProto or StaticData, not artifact vocabulary. It lives in
 * bytecode/ with its siblings.
 *
 * ─── Design: schema knows its shape, not its storage ──────────────────────
 * A schema answers "how many columns, of what types, with what
 * attributes, at the table level and per column." It does not answer
 * "where are the rows" (that's TableObject's job), "what are the rows"
 * (BakedTable's seed rows, or TableObject's live rows), or "what is
 * this table named" (BakedTable::mangledName, a tooling concern).
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * contract/TypeDescriptor.hpp.
 */

#pragma once

#include "contract/TypeDescriptor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lucid::bytecode {

/// @brief One column of a table.
///
/// A column's shape is its name, its type, and its per-column
/// attributes. The name is mangled (module-qualified) so it is unique
/// across the whole artifact; the unqualified name is recoverable from
/// the mangled one if a tool needs it.
struct TableColumn {
    /// The column's mangled name. Matches the mangling the compiler
    /// assigns in EmitDecl; the runtime never interprets it.
    std::string mangledName;

    /// The column's type. A column's type is fixed at declaration
    /// (grammar §4.1.3) and is a primitive, a host type, a row
    /// reference, an array, or a function type — never a bare table
    /// type.
    contract::TypeDescriptor type;

    /// True if the column carries @unique: no two rows may share a
    /// value in this column. Checked on ADD and on every write.
    bool isUnique = false;

    /// True if the column carries @primary. Implies isUnique. At most
    /// one column per table may be @primary.
    bool isPrimary = false;

    /// True if the column carries @readonly: cells may not be written
    /// after the row is added.
    bool isReadonly = false;
};

/// @brief The shape of one table.
///
/// A TableSchema is a plain value. The compiler produces one from a
/// TableDeclAST and bakes it into StaticData. The interpreter's loader
/// reads it and uses it to construct a TableObject. The TableObject
/// holds a const TableSchema* for the duration of its life.
struct TableSchema {
    /// The table's columns, in source order. A table with zero columns
    /// is legal only if it is host-backed (see isHostBacked); the
    /// compiler's post-bake assertions enforce this.
    std::vector<TableColumn> columns;

    /// True if the table carries @fixed: the row set is decided at
    /// declaration; no ADD, REMOVE, CLEAR, or SHRINK. Cells remain
    /// writable.
    bool isFixed = false;

    /// True if the table carries @readonly: no mutation of any kind.
    /// Implies everything @fixed forbids, plus frozen cells.
    bool isReadonly = false;

    /// True if the table carries @packed: contiguous storage, no slack;
    /// array-typed cells cannot be reassigned. Requires isFixed or
    /// isReadonly (grammar §4.1.4).
    bool isPacked = false;

    /// True if the table carries @columnar: columns stored in separate
    /// contiguous buffers rather than row-major.
    bool isColumnar = false;

    /// True if the table carries @request. Only legal on a host-backed
    /// table; marks it as a single-operation async handle usable with
    /// waitForRequest (grammar §9.2.3).
    bool isRequest = false;

    /// True if the table was declared TABLE X = host("..."). The
    /// columns vector is empty for a host-backed table (the host's
    /// type registration defines its shape).
    bool isHostBacked = false;

    /// Only meaningful if isHostBacked. The index into the artifact's
    /// HostSymbolTable that names this host type. -1 otherwise.
    int32_t hostTypeSymbolIndex = -1;

    /// The @reserve(N) hint, if present. Only meaningful on a growing
    /// (non-fixed, non-readonly, non-packed) table; the compiler
    /// rejects it on any other kind (grammar §4.1.4).
    std::optional<uint64_t> reservedCount;

    // ─── Queries ────────────────────────────────────────────────────────

    /// The number of columns.
    uint32_t columnCount() const noexcept {
        return static_cast<uint32_t>(columns.size());
    }

    /// The index of the @primary column, or UINT32_MAX if none.
    /// Precondition (asserted in debug): at most one column has
    /// isPrimary.
    uint32_t primaryColumnIndex() const noexcept {
        for (uint32_t i = 0; i < columns.size(); ++i) {
            if (columns[i].isPrimary) return i;
        }
        return UINT32_MAX;
    }

    /// True if any column has @primary.
    bool hasPrimary() const noexcept {
        return primaryColumnIndex() != UINT32_MAX;
    }

    /// True if the table's row set is fixed: no ADD, REMOVE, CLEAR, or
    /// SHRINK. True when isFixed, isReadonly, or isPacked is set — any
    /// one of the three makes the row set fixed (grammar §4.1.1).
    bool hasFixedRowSet() const noexcept {
        return isFixed || isReadonly || isPacked;
    }

    /// True if cells may be written. False for @readonly tables.
    bool cellsWritable() const noexcept {
        return !isReadonly;
    }
};

} // namespace lucid::bytecode