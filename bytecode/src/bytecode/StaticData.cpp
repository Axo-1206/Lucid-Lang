/// @file bytecode/StaticData.cpp
/// @brief Invariant checking for the baked initial-state data.

#include "bytecode/StaticData.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <vector>

using namespace lucid::contract;

namespace lucid::bytecode {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Constant/type agreement
// ─────────────────────────────────────────────────────────────────────────────
//
// A binding's declared type and its baked initial value must describe
// the same shape. This is the check that catches a compiler mis-wiring
// (an Int binding given a String value, an Array binding given a
// scalar, a Nil value for a non-nullable type).
//
// The check is structural but bounded:
//
//   - it descends through a Nullable to its inner type (nil is always
//     acceptable for a nullable; a non-nil value must match the inner
//     type),
//   - it checks the element kind of an Array (the elements themselves
//     are not re-checked structurally, to keep the cost linear),
//   - it does not descend into host types or function types — the
//     compiler cannot see a host type's shape, and a function constant
//     carries only a code address, not a signature.

void checkConstantMatchesType(const Constant& value,
                              const TypeDescriptor& type) {
    using CK = Constant::Kind;
    using TK = TypeDescriptor::Kind;

    // A Nullable accepts Nil, or a value matching the inner type.
    if (type.kind == TK::Nullable) {
        if (value.kind == CK::Nil) return;
        AST_ASSERT_MSG(type.component != nullptr,
            "StaticData: a Nullable type has no inner type — "
            "the type descriptor was not built correctly");
        checkConstantMatchesType(value, *type.component);
        return;
    }

    // Nil is only legal for a Nullable, which was handled above.
    AST_ASSERT_MSG(value.kind != CK::Nil,
        "StaticData: a non-nullable binding has a Nil initial value — "
        "Sema should have rejected this declaration");

    switch (type.kind) {
        case TK::Primitive: {
            switch (type.primitive) {
                case PrimitiveKind::Bool:
                    AST_ASSERT_MSG(value.kind == CK::Bool,
                        "StaticData: a Bool binding has a non-Bool "
                        "initial value");
                    return;
                case PrimitiveKind::String:
                    AST_ASSERT_MSG(value.kind == CK::String,
                        "StaticData: a String binding has a non-String "
                        "initial value");
                    return;
                case PrimitiveKind::Char:
                    AST_ASSERT_MSG(value.kind == CK::Char,
                        "StaticData: a Char binding has a non-Char "
                        "initial value");
                    return;
                case PrimitiveKind::Int8:
                case PrimitiveKind::Int16:
                case PrimitiveKind::Int32:
                case PrimitiveKind::Int64:
                case PrimitiveKind::Uint8:
                case PrimitiveKind::Uint16:
                case PrimitiveKind::Uint32:
                case PrimitiveKind::Uint64:
                    AST_ASSERT_MSG(value.kind == CK::Int,
                        "StaticData: an integer binding has a non-Int "
                        "initial value");
                    return;
                case PrimitiveKind::Float32:
                case PrimitiveKind::Float64:
                    AST_ASSERT_MSG(value.kind == CK::Float,
                        "StaticData: a float binding has a non-Float "
                        "initial value");
                    return;
                case PrimitiveKind::Void:
                    // A Void binding cannot have a value at all;
                    // Sema rejects a `let x: void` declaration.
                    AST_ASSERT_MSG(false,
                        "StaticData: a Void binding reached the "
                        "invariant check — Sema should have rejected "
                        "this declaration");
                    return;
            }
            return;
        }
        case TK::Array: {
            AST_ASSERT_MSG(value.kind == CK::Array,
                "StaticData: an Array binding has a non-Array "
                "initial value");
            if (type.arrayKind == ArrayKind::Fixed) {
                AST_ASSERT_MSG(
                    std::get<std::vector<Constant>>(value.value).size()
                        == type.fixedSize,
                    "StaticData: a fixed-array binding's initial value "
                    "has the wrong element count");
            }
            return;
        }
        case TK::RowRef:
            AST_ASSERT_MSG(value.kind == CK::RowRef,
                "StaticData: a RowRef binding has a non-RowRef "
                "initial value");
            return;
        case TK::Function:
            AST_ASSERT_MSG(value.kind == CK::Function,
                "StaticData: a Function binding has a non-Function "
                "initial value");
            return;
        case TK::Named:
            // A Named type is a table reference or a host handle. A
            // table reference is not a value a binding can hold (a
            // bare table name is a compile-time identity, not a
            // runtime value); a host handle has no compile-time
            // constant representation. Either way, a Named binding
            // should never reach here — Sema rejects it.
            AST_ASSERT_MSG(false,
                "StaticData: a Named-type binding reached the "
                "invariant check — Sema should have rejected this "
                "declaration");
            return;
        case TK::Nullable:
            // Handled above; unreachable.
            return;
        case TK::Unknown:
            AST_ASSERT_MSG(false,
                "StaticData: an Unknown type reached the invariant "
                "check — Sema should have rejected this declaration");
            return;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Invariants
// ─────────────────────────────────────────────────────────────────────────────

void StaticData::checkInvariants() const {
    // ─── Tables ────────────────────────────────────────────────────────
    //
    // A BakedTable is (mangledName, TableSchema schema, rows). Every
    // table-shape invariant is a property of the schema; the rows are
    // checked against the schema's column count.
    for (size_t ti = 0; ti < m_tables.size(); ++ti) {
        const BakedTable& table = m_tables[ti];
        const TableSchema& schema = table.schema;

        AST_ASSERT_MSG(!table.mangledName.empty(),
            "StaticData: a BakedTable has an empty mangled name");

        // A non-host-backed table must have at least one column. A
        // host-backed table has no columns by construction (its shape
        // is the host type).
        if (!schema.isHostBacked) {
            AST_ASSERT_MSG(!schema.columns.empty(),
                "StaticData: a columned table has no columns");
        } else {
            AST_ASSERT_MSG(schema.columns.empty(),
                "StaticData: a host-backed table has columns — "
                "the compiler should not have emitted any");
            AST_ASSERT_MSG(schema.hostTypeSymbolIndex >= 0,
                "StaticData: a host-backed table has no host type "
                "symbol index");
        }

        // Every row has one cell per column. A mismatch is a compiler
        // bug: the row was baked with the wrong cell count.
        const size_t columnCount = schema.columns.size();
        for (size_t ri = 0; ri < table.rows.size(); ++ri) {
            const auto& row = table.rows[ri];
            AST_ASSERT_MSG(row.size() == columnCount,
                "StaticData: a table row has a cell count that does not "
                "match the table's column count");
        }

        // A @fixed or @readonly table must have at least one row
        // (§4.1.1b). Sema rejects the empty case, but the assert
        // re-checks because a malformed Sema output must not silently
        // produce an empty fixed table.
        if (schema.isFixed || schema.isReadonly) {
            AST_ASSERT_MSG(!table.rows.empty(),
                "StaticData: a @fixed/@readonly table has no rows — "
                "Sema should have rejected this declaration");
        }

        // @packed requires @fixed or @readonly. Sema checks this; the
        // assert re-checks the compiler's output. This is also the
        // reason TableSchema::hasFixedRowSet() does not include
        // `|| isPacked`: a @packed table without @fixed/@readonly is
        // a compiler bug, and this assert catches it before any query
        // consults hasFixedRowSet.
        if (schema.isPacked) {
            AST_ASSERT_MSG(schema.isFixed || schema.isReadonly,
                "StaticData: a @packed table is neither @fixed nor "
                "@readonly — Sema should have rejected this declaration");
        }

        // @fixed and @readonly are mutually exclusive. They express
        // different things: @fixed fixes the row set (cells writable);
        // @readonly fixes the row set and freezes the cells. Grammar
        // §4.1.1 makes them exclusive; a table with both is a
        // compiler bug.
        AST_ASSERT_MSG(!(schema.isFixed && schema.isReadonly),
            "StaticData: a table is both @fixed and @readonly — "
            "the grammar makes them mutually exclusive, and Sema "
            "should have rejected the declaration");

        // @request is only valid on a host-backed table.
        if (schema.isRequest) {
            AST_ASSERT_MSG(schema.isHostBacked,
                "StaticData: a @request table is not host-backed — "
                "Sema should have rejected this declaration");
        }

        // @reserve is only meaningful on a growing table (grammar
        // §4.1.4). A @fixed, @readonly, or @packed table has a fixed
        // row set; reserving capacity for rows that can never be
        // added is meaningless.
        if (schema.reservedCount.has_value()) {
            AST_ASSERT_MSG(!schema.hasFixedRowSet(),
                "StaticData: a @reserve annotation appears on a table "
                "with a fixed row set — the grammar only allows "
                "@reserve on a growing table, and Sema should have "
                "rejected this declaration");
        }

        // A host-backed table has no rows of its own: its shape is
        // the host type, and rows live in the host. The compiler
        // should never bake rows for a host-backed table.
        if (schema.isHostBacked) {
            AST_ASSERT_MSG(table.rows.empty(),
                "StaticData: a host-backed table has rows — "
                "the compiler should not have baked any");
        }

        // @primary: at most one column per table.
        int primaryCount = 0;
        for (const auto& col : schema.columns) {
            if (col.isPrimary) ++primaryCount;
        }
        AST_ASSERT_MSG(primaryCount <= 1,
            "StaticData: a table has more than one @primary column — "
            "Sema should have rejected this declaration");
    }

    // ─── Table mangled-name uniqueness ─────────────────────────────────
    //
    // The Manifest checks that the mangled names it lists are globally
    // unique, but it only checks the names it knows about. A duplicate
    // within StaticData::tables that the Manifest does not list would
    // slip past. This is a belt-and-braces check: the compiler's pass A
    // is the only writer, and it uses emplace() with an inserted
    // assertion, so a duplicate here means the tables list was mutated
    // outside the compiler's control flow.
    for (size_t i = 0; i < m_tables.size(); ++i) {
        for (size_t j = i + 1; j < m_tables.size(); ++j) {
            AST_ASSERT_MSG(
                m_tables[i].mangledName != m_tables[j].mangledName,
                "StaticData: two tables share a mangled name — "
                "the compiler's table registration is not producing "
                "unique names");
        }
    }

    // ─── Bindings ──────────────────────────────────────────────────────
    for (size_t i = 0; i < m_bindings.size(); ++i) {
        const BakedBinding& b = m_bindings[i];

        AST_ASSERT_MSG(!b.mangledName.empty(),
            "StaticData: a BakedBinding has an empty mangled name");

        checkConstantMatchesType(b.initialValue, b.type);
    }
}

} // namespace lucid::bytecode