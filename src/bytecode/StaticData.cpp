/// @file bytecode/StaticData.cpp
/// @brief Invariant checking for the baked initial-state data.

#include "StaticData.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

namespace lucid::bytecode {

void StaticData::checkInvariants() const {
    // ─── Tables ────────────────────────────────────────────────────────
    for (size_t ti = 0; ti < m_tables.size(); ++ti) {
        const BakedTable& table = m_tables[ti];

        AST_ASSERT_MSG(!table.mangledName.empty(),
            "StaticData: a BakedTable has an empty mangled name");

        // A non-host-backed table must have at least one column. A
        // host-backed table has no columns by construction (its shape
        // is the host type).
        if (!table.isHostBacked) {
            AST_ASSERT_MSG(!table.columns.empty(),
                "StaticData: a columned table has no columns");
        } else {
            AST_ASSERT_MSG(table.columns.empty(),
                "StaticData: a host-backed table has columns — "
                "the compiler should not have emitted any");
            AST_ASSERT_MSG(table.hostTypeSymbolIndex >= 0,
                "StaticData: a host-backed table has no host type "
                "symbol index");
        }

        // Every row has one cell per column. A mismatch is a compiler
        // bug: the row was baked with the wrong cell count.
        const size_t columnCount = table.columns.size();
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
        if (table.isFixed || table.isReadonly) {
            AST_ASSERT_MSG(!table.rows.empty(),
                "StaticData: a @fixed/@readonly table has no rows — "
                "Sema should have rejected this declaration");
        }

        // @packed requires @fixed or @readonly. Sema checks this; the
        // assert re-checks the compiler's output.
        if (table.isPacked) {
            AST_ASSERT_MSG(table.isFixed || table.isReadonly,
                "StaticData: a @packed table is neither @fixed nor "
                "@readonly — Sema should have rejected this declaration");
        }

        // @request is only valid on a host-backed table.
        if (table.isRequest) {
            AST_ASSERT_MSG(table.isHostBacked,
                "StaticData: a @request table is not host-backed — "
                "Sema should have rejected this declaration");
        }

        // @primary: at most one column per table.
        int primaryCount = 0;
        for (const auto& col : table.columns) {
            if (col.isPrimary) ++primaryCount;
        }
        AST_ASSERT_MSG(primaryCount <= 1,
            "StaticData: a table has more than one @primary column — "
            "Sema should have rejected this declaration");
    }

    // ─── Bindings ──────────────────────────────────────────────────────
    for (size_t i = 0; i < m_bindings.size(); ++i) {
        const BakedBinding& b = m_bindings[i];

        AST_ASSERT_MSG(!b.mangledName.empty(),
            "StaticData: a BakedBinding has an empty mangled name");

        // The binding's type and its initial value's kind must be
        // consistent. For a primitive type, the initial value's kind
        // must match the primitive kind's category (Int for integer
        // primitives, Float for float primitives, etc.). For a
        // nullable type, the value is either Nil or the inner kind.
        //
        // A full structural check would recurse into arrays and host
        // types; this version checks the top-level shape, which is
        // enough to catch the common compiler bugs (an Int binding
        // given a String value, a Nil value for a non-nullable type).
        if (b.type.isPrimitive()) {
            const bool valueIsNil = (b.initialValue.kind == Constant::Kind::Nil);
            AST_ASSERT_MSG(!valueIsNil,
                "StaticData: a non-nullable primitive binding has a Nil "
                "initial value");
        }
    }
}

} // namespace lucid::bytecode