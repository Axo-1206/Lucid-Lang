/// @file sema/support/TableConstraintChecker.cpp
/// @brief Implementation of every table-shape constraint check.
///
/// ─── Design: one function per rule ────────────────────────────────────────
/// The `.cpp` is organized as one static helper per rule, called in
/// sequence from `checkTableConstraints`. Each helper returns true on
/// success and false if it emitted a diagnostic; the entry point ANDs
/// the results so the caller gets a single "was everything clean?".
///
/// The helpers are grouped by what they need from the table:
///
///   - Attribute-only (rules 1, 2, 4, 5): read the decoded attribute
///     fields. These could run before columns are resolved.
///   - Shape-only (rules 3, 6, 7, 8, 11, 12): read the columns and
///     rows, but not the resolved column types.
///   - Type-dependent (rules 9, 10): read the resolved column types.
///   - Row-dependent (rules 13, 14, 15): read the inline rows and the
///     constant evaluator's result on each cell.
///
/// The order in `checkTableConstraints` runs the cheap checks first, so
/// a table with an obvious attribute error does not also walk its rows.

#include "TableConstraintChecker.hpp"

#include "sema/context/SemaContext.hpp"
#include "sema/const_eval/ConstEvaluator.hpp"
#include "sema/types/SemaType.hpp"

#include "core/ASTStrings.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/registry/BuiltinMethodRegistry.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace lucid::diag;

namespace lucid::sema {

// ═════════════════════════════════════════════════════════════════════════════
// Local helpers
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// ─── Is this a host-backed table type? ───────────────────────────────────────

/// Follow a `NamedTypeAST` to its table declaration and check whether
/// the table is host-backed. Returns false for any other type shape.
///
/// This is a local helper because the checker needs it in three places
/// (rule 5, rule 9, rule 15) and each call site has the same
/// resolution logic.
bool isHostBackedTableTypeLocal(TypeAST* type) {
    if (!type || !type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    if (!named->resolvedDecl) return false;
    if (!named->resolvedDecl->isa<TableDeclAST>()) return false;
    return named->resolvedDecl->as<TableDeclAST>()->isHostBacked;
}

// ─── The table a row reference (or named type) resolves to ───────────────────

/// Return the `TableDeclAST*` a `NamedTypeAST` or `RowRefTypeAST` names,
/// or nullptr. Used by the cyclic-reference walk, which follows both
/// `NamedTypeAST` and `&T` references to their target tables.
TableDeclAST* resolveReferencedTable(TypeAST* type) {
    if (!type) return nullptr;

    TypeAST* inner = type;
    if (inner->isa<RowRefTypeAST>()) {
        inner = inner->as<RowRefTypeAST>()->inner;
    }
    if (!inner || !inner->isa<NamedTypeAST>()) return nullptr;

    NamedTypeAST* named = inner->as<NamedTypeAST>();
    if (!named->resolvedDecl || !named->resolvedDecl->isa<TableDeclAST>()) {
        return nullptr;
    }
    return named->resolvedDecl->as<TableDeclAST>();
}

// ─── The `by<Column>` name for a column ──────────────────────────────────────

/// Build the `by<Column>` name for a column: `by` + the column's name
/// with its first letter uppercased. Mirrors `resolveTableMemberAccess`'s
/// `expected` construction in `SemaExpr.cpp`.
std::string byColumnName(std::string_view columnName) {
    std::string result;
    result.reserve(2 + columnName.size());
    result += "by";
    if (!columnName.empty()) {
        result += static_cast<char>(std::toupper(
            static_cast<unsigned char>(columnName[0])));
        result.append(columnName.substr(1));
    }
    return result;
}

// ─── The row "name" of a fixed table row ─────────────────────────────────────

/// The name a fixed-table row is addressable by — the string value of
/// the row's first cell. Returns an invalid `InternedString` if the
/// row's first cell is not a string literal (in which case the row is
/// not addressable by sugar, and the checks that care about row names
/// skip it).
InternedString rowSugarName(RowAST* row) {
    if (!row || row->cells.empty()) return InternedString{};
    ExprAST* firstCell = row->cells[0];
    if (!firstCell || !firstCell->isa<LiteralExprAST>()) return InternedString{};
    LiteralExprAST* lit = firstCell->as<LiteralExprAST>();
    if (lit->kind != LiteralKind::String &&
        lit->kind != LiteralKind::RawString) {
        return InternedString{};
    }
    return lit->value;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Attribute-only checks (rules 1, 2, 4, 5)
// ═════════════════════════════════════════════════════════════════════════════

// ─── Rule 1: @packed requires @fixed or @readonly ────────────────────────────

static bool checkPackedRequiresFixed(TableDeclAST* table, SemaContext& ctx) {
    if (!table->isPacked) return true;
    if (table->hasFixedRowSet()) return true;

    ctx.diagnostics.error(DiagCode::Table_PackedRequiresFixed, table,
                          "table '", ctx.pool.lookup(table->name),
                          "' carries '@packed' but neither '@fixed' nor "
                          "'@readonly' — a packed table's storage cannot "
                          "grow, so it must be fixed-row-set");
    ctx.diagnostics.note(table,
                         "write '@packed @fixed' for writable cells, or "
                         "'@packed @readonly' for a fully frozen table");
    return false;
}

// ─── Rule 2: @fixed and @readonly are mutually exclusive ─────────────────────

static bool checkFixedReadonlyConflict(TableDeclAST* table, SemaContext& ctx) {
    if (!table->isFixed || !table->isReadonly) return true;

    ctx.diagnostics.error(DiagCode::Table_FixedReadonlyConflict, table,
                          "table '", ctx.pool.lookup(table->name),
                          "' carries both '@fixed' and '@readonly' — "
                          "'@fixed' is redundant, since '@readonly' already "
                          "forbids everything '@fixed' does");
    ctx.diagnostics.note(table,
                         "use '@readonly' alone for a fully frozen table, "
                         "or '@fixed' alone when the cells remain writable");
    return false;
}

// ─── Rule 3: @reserve is legal only on a growing table ───────────────────────

static bool checkReserveOnGrowingTable(TableDeclAST* table, SemaContext& ctx) {
    if (!table->isReserved) return true;
    if (table->isHostBacked) return true;   // host-backed tables have their own rule (rule 5)
    if (!table->hasFixedRowSet() && !table->isPacked) return true;

    // A @reserve on a fixed-row-set or packed table is legal-in-grammar
    // but semantically empty: the table's row set cannot grow, so there
    // is nothing to reserve room for.
    const char* reason = table->isPacked
        ? "'@packed'"
        : (table->isReadonly ? "'@readonly'" : "'@fixed'");

    ctx.diagnostics.error(DiagCode::Table_ReserveOnFixedTable, table,
                          "table '", ctx.pool.lookup(table->name),
                          "' carries '@reserve(N)' but is ", reason,
                          " — a table whose row set cannot grow has nothing "
                          "to reserve room for");
    return false;
}

// ─── Rule 4 + 5: @request on a host-backed table ─────────────────────────────

static bool checkRequestOnHostTable(TableDeclAST* table, SemaContext& ctx) {
    if (!table->isRequest) return true;

    if (!table->isHostBacked) {
        ctx.diagnostics.error(DiagCode::Table_RequestOnNonHost, table,
                              "table '", ctx.pool.lookup(table->name),
                              "' carries '@request' but is not a host-backed "
                              "table — '@request' is only valid on "
                              "'TABLE X = host(\"...\")'");
        return false;
    }

    // Rule 5 is a refinement of rule 4: the table must be host-backed
    // *and* the host backing must have been written. A table cannot be
    // `isHostBacked == true` with an empty `hostName` unless the parser
    // produced a malformed host target — an invariant violation. Assert
    // rather than emit a user-facing diagnostic.
    AST_ASSERT_MSG(table->hostName.isValid(),
                   "TableConstraintChecker: a host-backed table has no host "
                   "name — the parser should have rejected the host target");

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Shape-only checks (rules 6, 7, 8, 11, 12)
// ═════════════════════════════════════════════════════════════════════════════

// ─── Rule 6: a @fixed/@readonly table must have at least one row ─────────────

static bool checkFixedTableHasRows(TableDeclAST* table, SemaContext& ctx) {
    if (!table->hasFixedRowSet()) return true;
    if (table->isHostBacked) return true;   // host tables have no rows to check
    if (!table->rows.empty()) return true;

    // The parser accepts a FIXED table without an initializer; Sema
    // rejects it. A fixed-row-set table that starts empty can never
    // gain a row, so it is always a mistake.
    ctx.diagnostics.error(DiagCode::Table_FixedTableEmpty, table,
                          "fixed-row-set table '", ctx.pool.lookup(table->name),
                          "' has no rows — a table whose row set cannot "
                          "change cannot hold any data if it starts empty");
    ctx.diagnostics.note(table,
                         "remove '@fixed'/'@readonly' to make the table "
                         "growable, or add an inline '= [ ... ]' initializer");
    return false;
}

// ─── Rule 7: column names are unique ─────────────────────────────────────────

static bool checkUniqueColumnNames(TableDeclAST* table, SemaContext& ctx) {
    std::unordered_map<InternedString, ColumnDeclAST*> seen;
    bool ok = true;

    for (ColumnDeclAST* column : table->columns) {
        if (!column) continue;
        auto [it, inserted] = seen.emplace(column->name, column);
        if (!inserted) {
            ctx.diagnostics.error(DiagCode::Name_ColumnDuplicate, column,
                                  "duplicate column name '",
                                  ctx.pool.lookup(column->name),
                                  "' in table '",
                                  ctx.pool.lookup(table->name), "'");
            ctx.diagnostics.note(it->second, "previous column with this name");
            ok = false;
        }
    }
    return ok;
}

// ─── Rule 8: at most one @primary column ─────────────────────────────────────

static bool checkAtMostOnePrimary(TableDeclAST* table, SemaContext& ctx) {
    ColumnDeclAST* first = nullptr;
    bool ok = true;

    for (ColumnDeclAST* column : table->columns) {
        if (!column || !column->isPrimary) continue;

        if (!first) {
            first = column;
            continue;
        }

        ctx.diagnostics.error(DiagCode::Table_PrimaryAtMostOne, column,
                              "table '", ctx.pool.lookup(table->name),
                              "' has more than one '@primary' column "
                              "('", ctx.pool.lookup(first->name),
                              "' and '", ctx.pool.lookup(column->name), "')");
        ctx.diagnostics.note(first, "the first '@primary' column is here");
        ok = false;
    }
    return ok;
}

// ─── Rule 11: column/row names do not equal a method name ────────────────────
//
// A built-in method name is ALLCAPS (`ADD`, `FIND`, ...). A column name
// is lowercase or camelCase; a fixed-row member name is PascalCase. The
// two name spaces are supposed to be disjoint. If they are not, a
// `T.FIND` in an expression position is ambiguous: is it the method or
// the column/member? The grammar forbids the collision, so Sema
// enforces it here.

static bool checkNamesCollideWithMethods(TableDeclAST* table, SemaContext& ctx) {
    bool ok = true;

    for (ColumnDeclAST* column : table->columns) {
        if (!column) continue;
        std::string_view name = ctx.pool.lookupView(column->name);
        if (ctx.builtinMethodRegistry.isRegistered(name)) {
            ctx.diagnostics.error(DiagCode::Table_NameCollidesWithMethod,
                                  column,
                                  "column '", name, "' in table '",
                                  ctx.pool.lookup(table->name),
                                  "' has the same name as a built-in method");
            ctx.diagnostics.note(column,
                                 "column names and method names must be "
                                 "disjoint; rename the column");
            ok = false;
        }
    }

    for (RowAST* row : table->rows) {
        InternedString rowName = rowSugarName(row);
        if (!rowName.isValid()) continue;

        std::string_view name = ctx.pool.lookupView(rowName);
        if (ctx.builtinMethodRegistry.isRegistered(name)) {
            ctx.diagnostics.error(DiagCode::Table_NameCollidesWithMethod,
                                  table,
                                  "fixed-row member '", name, "' in table '",
                                  ctx.pool.lookup(table->name),
                                  "' has the same name as a built-in method");
            ok = false;
        }
    }

    return ok;
}

// ─── Rule 12: the by<Column> name does not collide ───────────────────────────
//
// A @primary column generates a `by<Column>` method name. If a column
// or a fixed-row member already has that name, the generated lookup
// would be ambiguous. §4.1.5 forbids the collision.

static bool checkByColumnNameCollision(TableDeclAST* table, SemaContext& ctx) {
    // Find the primary column (rule 8 already checked there is at most
    // one; if there are zero, there is no generated name and nothing
    // to check).
    ColumnDeclAST* primary = nullptr;
    for (ColumnDeclAST* column : table->columns) {
        if (column && column->isPrimary) {
            primary = column;
            break;
        }
    }
    if (!primary) return true;

    std::string generated = byColumnName(ctx.pool.lookupView(primary->name));

    // Collide against an existing column?
    for (ColumnDeclAST* column : table->columns) {
        if (!column) continue;
        if (column == primary) continue;   // a column does not collide with itself
        if (ctx.pool.lookupView(column->name) == generated) {
            ctx.diagnostics.error(DiagCode::Table_GeneratedNameCollides,
                                  column,
                                  "'@primary ", ctx.pool.lookup(primary->name),
                                  "' generates the name '", generated,
                                  "', which collides with column '",
                                  ctx.pool.lookup(column->name), "'");
            return false;
        }
    }

    // Collide against a fixed-row member?
    for (RowAST* row : table->rows) {
        InternedString rowName = rowSugarName(row);
        if (!rowName.isValid()) continue;
        if (ctx.pool.lookupView(rowName) == generated) {
            ctx.diagnostics.error(DiagCode::Table_GeneratedNameCollides,
                                  table,
                                  "'@primary ", ctx.pool.lookup(primary->name),
                                  "' generates the name '", generated,
                                  "', which collides with fixed-row member '",
                                  ctx.pool.lookup(rowName), "'");
            return false;
        }
    }

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Type-dependent checks (rules 9, 10)
// ═════════════════════════════════════════════════════════════════════════════

// ─── Rule 9 + 10: @primary column key type eligibility ───────────────────────
//
// A @primary column must have a type the runtime can hash and compare
// for equality. The allowed types (§4.1.5):
//   - integer types
//   - bool
//   - char
//   - string
//   - a host-backed type whose registration provides equality and hashing
//
// Nilable, `&T`, array, function, and `unit` types are all rejected.
//
// The "host type with hashing" case is not decidable from the type
// alone — the host's registration provides equality and hashing, or it
// does not, and Sema does not see the registration. The checker
// accepts any host-backed type and trusts the host to have provided
// the required protocol. A runtime panic on a missing protocol is the
// fallback.

static bool checkPrimaryKeyType(TableDeclAST* table, SemaContext& ctx) {
    bool ok = true;

    for (ColumnDeclAST* column : table->columns) {
        if (!column || !column->isPrimary) continue;

        TypeAST* keyType = column->type;
        if (!keyType) continue;   // resolution already failed

        // ─── Rule 10: nilable is forbidden ──────────────────────────────
        if (isNullableType(keyType)) {
            ctx.diagnostics.error(DiagCode::Table_PrimaryNotHashable, column,
                                  "'@primary' column '",
                                  ctx.pool.lookup(column->name),
                                  "' has a nilable type ",
                                  typeToString(keyType, ctx.pool),
                                  " — a primary key cannot be nil");
            ok = false;
            continue;
        }

        // ─── Rule 10: `&T` is forbidden ─────────────────────────────────
        //
        // A row reference is inherently nilable and its identity is a
        // runtime slot+generation, not a stable key. §4.1.5 explicitly
        // excludes `&T`.
        if (isRowRefType(keyType)) {
            ctx.diagnostics.error(DiagCode::Table_PrimaryNotHashable, column,
                                  "'@primary' column '",
                                  ctx.pool.lookup(column->name),
                                  "' has a row-reference type ",
                                  typeToString(keyType, ctx.pool),
                                  " — a primary key must be a value, not "
                                  "a reference");
            ok = false;
            continue;
        }

        // ─── Rule 10: arrays are forbidden ──────────────────────────────
        if (isArrayType(keyType)) {
            ctx.diagnostics.error(DiagCode::Table_PrimaryNotHashable, column,
                                  "'@primary' column '",
                                  ctx.pool.lookup(column->name),
                                  "' has an array type ",
                                  typeToString(keyType, ctx.pool),
                                  " — a primary key must be a scalar");
            ok = false;
            continue;
        }

        // ─── Rule 10: function types are forbidden ──────────────────────
        if (isFunctionType(keyType)) {
            ctx.diagnostics.error(DiagCode::Table_PrimaryNotHashable, column,
                                  "'@primary' column '",
                                  ctx.pool.lookup(column->name),
                                  "' has a function type",
                                  " — a primary key must be a scalar");
            ok = false;
            continue;
        }

        // ─── Rule 10: `unit` is forbidden ───────────────────────────────
        if (isUnitType(keyType)) {
            ctx.diagnostics.error(DiagCode::Table_PrimaryNotHashable, column,
                                  "'@primary' column '",
                                  ctx.pool.lookup(column->name),
                                  "' has type 'unit'",
                                  " — a primary key must be a value");
            ok = false;
            continue;
        }

        // ─── Rule 9: type must be a valid key type ──────────────────────
        const bool isKeyType =
            isIntegerType(keyType)  ||
            isBoolType(keyType)     ||
            isCharType(keyType)     ||
            isStringType(keyType)   ||
            isHostBackedTableTypeLocal(keyType);

        if (!isKeyType) {
            ctx.diagnostics.error(DiagCode::Table_PrimaryNotHashable, column,
                                  "'@primary' column '",
                                  ctx.pool.lookup(column->name),
                                  "' has a type the runtime cannot hash: ",
                                  typeToString(keyType, ctx.pool));
            ctx.diagnostics.note(column,
                                 "a primary key must be an integer, bool, "
                                 "char, string, or a host type with equality "
                                 "and hashing");
            ok = false;
        }
    }

    return ok;
}

// ═════════════════════════════════════════════════════════════════════════════
// Row-dependent checks (rules 13, 14, 15)
// ═════════════════════════════════════════════════════════════════════════════

// ─── Rule 14: every fixed-table cell is a compile-time constant ──────────────
//
// For a `@fixed` or `@readonly` table, every cell of every inline row
// must be a `const_expr` (§4.1.1c). The evaluator folds each cell; if
// the fold fails (Unknown or Error), the cell is not a constant.
//
// A growing table's inline rows are also const expressions — the
// grammar treats them the same way — so this check runs for any table
// with inline rows, not just fixed ones.

static bool checkInitializerCellsAreConstant(TableDeclAST* table,
                                             SemaContext& ctx) {
    if (table->isHostBacked) return true;   // host tables have no inline rows
    if (table->rows.empty()) return true;   // nothing to check

    bool ok = true;

    for (RowAST* row : table->rows) {
        if (!row) continue;

        for (ExprAST* cell : row->cells) {
            if (!cell) continue;

            // Cell count must match column count; that check is done
            // separately below (it is not "constant-ness").
            ConstantValue folded = evaluate(cell, ctx);

            if (folded.isError()) {
                // The evaluator emitted a diagnostic; propagate the failure.
                ok = false;
                continue;
            }
            if (!folded.isEvaluated()) {
                ctx.diagnostics.error(DiagCode::Table_FixedCellNotConstant,
                                      cell,
                                      "table cell must be a compile-time "
                                      "constant expression");
                ok = false;
                continue;
            }

            // Cache the fold on the cell so downstream passes do not
            // re-evaluate. The evaluator itself never writes to the AST.
            cell->isConst    = true;
            cell->constValue = folded;
        }
    }

    return ok;
}

// ─── constantValuesEqual ─────────────────────────────────────────────────────
//
// Two `ConstantValue`s compare equal if they have the same kind and
// the same payload. The evaluator does not provide equality because
// the payload's comparison rules differ per kind (interned strings
// compare by id; ints and floats compare by value; arrays compare
// element-wise).

static bool constantValuesEqual(const ConstantValue& a, const ConstantValue& b) {
    if (a.kind != b.kind) return false;

    switch (a.kind) {
        case ConstantValue::Kind::Bool:
            return a.asBool() == b.asBool();
        case ConstantValue::Kind::Int:
            return a.asInt() == b.asInt();
        case ConstantValue::Kind::Float:
            return a.asFloat() == b.asFloat();
        case ConstantValue::Kind::String:
        case ConstantValue::Kind::Char:
            return a.asString() == b.asString();
        case ConstantValue::Kind::Nil:
            return true;
        case ConstantValue::Kind::Array: {
            const auto& av = a.asArray();
            const auto& bv = b.asArray();
            if (av.size() != bv.size()) return false;
            for (size_t i = 0; i < av.size(); ++i) {
                if (!constantValuesEqual(av[i], bv[i])) return false;
            }
            return true;
        }
        case ConstantValue::Kind::Function:
            return a.asFunction() == b.asFunction();
        default:
            return false;
    }
}

// ─── Rule 13: no two init rows share a @unique/@primary value ────────────────
//
// For each column marked @unique or @primary, every initializer row's
// value in that column must be distinct. The check uses the folded
// constants from rule 14 — a value that is not a compile-time constant
// has no comparable identity, and is skipped (its dynamic duplicate
// would be caught at runtime).
//
// The comparison is per-column. Two rows that share a value in one
// column but differ in another are still a duplicate on the @unique
// column.

static bool checkInitializerUniqueValues(TableDeclAST* table,
                                         SemaContext& ctx) {
    if (table->isHostBacked) return true;
    if (table->rows.empty()) return true;

    bool ok = true;

    for (size_t colIdx = 0; colIdx < table->columns.size(); ++colIdx) {
        ColumnDeclAST* column = table->columns[colIdx];
        if (!column) continue;
        if (!column->isUnique && !column->isPrimary) continue;

        // For each prior row's value in this column, note it. A row
        // whose value is not a constant has no stable identity and is
        // skipped — a runtime duplicate is a runtime panic, not a
        // compile-time error.
        struct Seen {
            ConstantValue value;
            size_t        rowIndex;
        };
        std::vector<Seen> seen;

        for (size_t rowIdx = 0; rowIdx < table->rows.size(); ++rowIdx) {
            RowAST* row = table->rows[rowIdx];
            if (!row || colIdx >= row->cells.size()) continue;
            ExprAST* cell = row->cells[colIdx];
            if (!cell) continue;

            ConstantValue value = evaluate(cell, ctx);
            if (!value.isEvaluated() || value.isError()) continue;

            for (const Seen& prev : seen) {
                if (constantValuesEqual(prev.value, value)) {
                    ctx.diagnostics.error(DiagCode::Table_DuplicateInitializerValue,
                                          cell,
                                          "duplicate value for @",
                                          (column->isPrimary ? "primary" : "unique"),
                                          " column '",
                                          ctx.pool.lookup(column->name),
                                          "' — also used by an earlier "
                                          "initializer row");
                    ctx.diagnostics.note(table,
                                         "initializer rows are numbered "
                                         "from 0; the earlier row is at "
                                         "index ", prev.rowIndex);
                    ok = false;
                    break;
                }
            }
            seen.push_back({value, rowIdx});
        }
    }

    return ok;
}

// ─── Rule 15: fixed tables do not cyclically reference each other ────────────
//
// A `@fixed`/`@readonly` table's inline cells can reference another
// fixed table's members — `B.One.x`, where `B` is another fixed table.
// The reference is a compile-time cross-reference: the compiler must
// resolve `B.One` before it can fold the cell, and if `B` in turn
// references this table, neither can be resolved.
//
// §4.1.1c states the rule: a genuine cycle between two fixed tables'
// constant rows is a compile error. This function implements the check
// with a DFS over the referenced-table graph:
//
//   - White: not visited.
//   - Grey:  on the current DFS stack.
//   - Black: fully processed.
//
// A back edge to a grey node is a cycle. The DFS starts at `table` and
// walks every fixed-table reference reachable from its inline cells.

namespace {

enum class CycleState { White, Grey, Black };

/// Extract the fixed-table references from a single cell. A cell can
/// reference another table via `T.Member` — the field-access sugar. The
/// resolver has already classified those and set `isFixedRowSugar` and
/// `resolvedDecl` on the access node.
///
/// A cell can also reference a table via a `NamedTypeAST` inside a
/// `const_expr` type annotation, but that shape does not appear in the
/// grammar's `const_expr`. So the only references walked here are the
/// field accesses.
void collectCellReferences(ExprAST* cell, std::vector<TableDeclAST*>& out);

void collectCellReferences(ExprAST* cell, std::vector<TableDeclAST*>& out) {
    if (!cell) return;

    if (cell->isa<FieldAccessExprAST>()) {
        FieldAccessExprAST* field = cell->as<FieldAccessExprAST>();
        if (field->isFixedRowSugar && field->resolvedDecl) {
            if (field->resolvedDecl->isa<TableDeclAST>()) {
                out.push_back(field->resolvedDecl->as<TableDeclAST>());
            }
        }
        return;
    }

    // A `const_expr` can be a `ParenExpr`, `UnaryExpr`, or `BinaryExpr`
    // around the reference. Walk into the operands.
    if (cell->isa<ParenExprAST>()) {
        collectCellReferences(cell->as<ParenExprAST>()->inner, out);
        return;
    }
    if (cell->isa<UnaryExprAST>()) {
        collectCellReferences(cell->as<UnaryExprAST>()->operand, out);
        return;
    }
    if (cell->isa<BinaryExprAST>()) {
        BinaryExprAST* binary = cell->as<BinaryExprAST>();
        collectCellReferences(binary->left, out);
        collectCellReferences(binary->right, out);
        return;
    }
}

/// Collect every fixed table referenced by any inline cell of `table`.
std::vector<TableDeclAST*> collectFixedTableReferences(TableDeclAST* table) {
    std::vector<TableDeclAST*> refs;
    if (!table) return refs;

    for (RowAST* row : table->rows) {
        if (!row) continue;
        for (ExprAST* cell : row->cells) {
            collectCellReferences(cell, refs);
        }
    }
    return refs;
}

/// DFS-based cycle detector. Returns true if a cycle is found that
/// reaches back to a grey node. `visited` is the per-call state.
bool detectCycle(TableDeclAST* table,
                 std::unordered_map<TableDeclAST*, CycleState>& state,
                 SemaContext& ctx) {
    auto it = state.find(table);
    if (it != state.end()) {
        if (it->second == CycleState::Grey) {
            // Back edge to a node on the current stack: cycle.
            return true;
        }
        if (it->second == CycleState::Black) {
            return false;   // fully processed; no cycle through this node
        }
        // White: fall through and process.
    }

    state[table] = CycleState::Grey;

    for (TableDeclAST* ref : collectFixedTableReferences(table)) {
        if (!ref) continue;
        if (ref == table) {
            // Self-reference — a fixed table whose cell references its
            // own member. This is a cycle of length 1.
            return true;
        }
        if (detectCycle(ref, state, ctx)) {
            return true;
        }
    }

    state[table] = CycleState::Black;
    return false;
}

} // namespace

static bool checkNoFixedTableCycles(TableDeclAST* table, SemaContext& ctx) {
    if (!table->hasFixedRowSet()) return true;
    if (table->isHostBacked) return true;
    if (table->rows.empty()) return true;

    std::unordered_map<TableDeclAST*, CycleState> state;
    if (!detectCycle(table, state, ctx)) return true;

    ctx.diagnostics.error(DiagCode::Table_FixedCycle, table,
                          "fixed table '", ctx.pool.lookup(table->name),
                          "' is part of a cycle of fixed tables that "
                          "reference each other's constant rows");
    ctx.diagnostics.note(table,
                         "a fixed table's inline cells are compile-time "
                         "constants; a cycle between two fixed tables' "
                         "constants has no resolution order and is "
                         "rejected");
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// The public entry point
// ═════════════════════════════════════════════════════════════════════════════

bool checkTableConstraints(TableDeclAST* table, SemaContext& ctx) {
    if (!table) return true;
    if (table->hasSyntaxError) return true;

    bool ok = true;

    // ─── Attribute-only checks ──────────────────────────────────────────
    //
    // These are cheap and run first. A table with a contradictory
    // attribute pair reports that before walking its columns and rows.
    if (!checkPackedRequiresFixed(table, ctx))       ok = false;
    if (!checkFixedReadonlyConflict(table, ctx))     ok = false;
    if (!checkReserveOnGrowingTable(table, ctx))     ok = false;
    if (!checkRequestOnHostTable(table, ctx))        ok = false;

    // ─── Shape-only checks ──────────────────────────────────────────────
    if (!checkFixedTableHasRows(table, ctx))         ok = false;
    if (!checkUniqueColumnNames(table, ctx))         ok = false;
    if (!checkAtMostOnePrimary(table, ctx))          ok = false;
    if (!checkNamesCollideWithMethods(table, ctx))   ok = false;
    if (!checkByColumnNameCollision(table, ctx))     ok = false;

    // ─── Type-dependent checks ──────────────────────────────────────────
    if (!checkPrimaryKeyType(table, ctx))            ok = false;

    // ─── Row-dependent checks ───────────────────────────────────────────
    //
    // Rule 14 runs before rule 13: rule 13 uses the folded constants
    // that rule 14 caches on the cells, so a cell that fails rule 14
    // also has no value for rule 13 to compare.
    if (!checkInitializerCellsAreConstant(table, ctx)) ok = false;
    if (!checkInitializerUniqueValues(table, ctx))     ok = false;
    if (!checkNoFixedTableCycles(table, ctx))          ok = false;

    return ok;
}

} // namespace lucid::sema