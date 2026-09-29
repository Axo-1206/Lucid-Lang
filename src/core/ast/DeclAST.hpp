/**
 * @file DeclAST.hpp
 *
 * @responsibility The AST nodes for declarations. Three forms —
 *                 tables, functions, and variables — plus the nodes that
 *                 support them (columns, parameters, imports).
 *
 * @hierarchy BaseAST → DeclAST → ValueDeclAST / TypeDeclAST → [Concrete Nodes]
 *
 * ─── Design: three declaration forms, and only three ──────────────────────
 * Every name a program introduces comes from one of:
 *
 *   - `TABLE X { ... }`       a table (in the type namespace)
 *   - `FN x(...) -> T { ... }` a function (in the value namespace)
 *   - `let x: T = expr`        a variable (in the value namespace)
 *
 * `import` is a directive, not a declaration; it introduces an alias
 * into the module's namespace but does not bind a value or a type.
 *
 * There is no `struct`, no `enum`, no `trait`, no `DEF`, no `satisfy`,
 * no `TYPE` alias declaration. Tables absorb both structs and enums;
 * function values absorb the rest.
 *
 * ─── Design: attribute span is the source of truth ────────────────────────
 * The parser produces an `ArenaSpan<AttributeAST*>` on every
 * declaration. Sema reads the span, interprets each attribute, and
 * writes a small set of decoded fields on the declaration node (for
 * example `TableDeclAST::readonly`). Later passes read the decoded
 * fields rather than re-walking the span, so the interpretation happens
 * once.
 *
 * The decoded fields are a cache, not a second source of truth. If the
 * span and the decoded field ever disagree, that's a Sema bug, and an
 * assertion catches it.
 *
 * ─── Design: the parameters of a function declaration are named ───────────
 * A `FN` declaration's parameters carry names (`a: int, b: string`) and
 * are stored as `ArenaSpan<ParamAST*>`. A function *type*'s parameters
 * are unnamed and are stored as `ArenaSpan<TypeAST*>` on
 * `FunctionTypeAST` (see TypeAST.hpp). The two are distinct nodes for a
 * reason: a declaration is a binding, a type is a shape.
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ImportDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An `import` directive: `import a.b.c [as d]`.
///
/// The path names the module to load. The alias is the local name used
/// to reach the module's exported members via `alias.Member`.
///
/// The parser does not resolve the path. The module's resolution is a
/// CLI concern (§3.1 of the grammar); the parser produces the AST node
/// and the CLI's import linker fills `ModuleAST::resolvedImports`.
///
/// @example
///   import core.math             → path = "core.math", alias = "math" (derived)
///   import core.math as math     → path = "core.math", alias = "math"
///   import entities.player as p  → path = "entities.player", alias = "p"
struct ImportDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImportDecl;

    const InternedString path;
    const InternedString alias;

    ImportDeclAST(InternedString p, InternedString a)
        : DeclAST(ASTKind::ImportDecl, a)
        , path(p)
        , alias(a) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ColumnDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One column of a table: `name: type`, with optional attributes.
///
/// A column is a typed slot in the table's shape. Columns are ordered;
/// the order is the order arguments to `T.ADD(...)` must be supplied in.
///
/// Attributes recognized on a column:
///   - `@unique`    — no two rows share a value in this column.
///   - `@primary`   — like `unique`, and enables `T.by<Column>(value)`.
///   - `@readonly`  — the column's cells cannot be written after a row
///                    is added.
///
/// `@primary` implies `@unique`; the parser stores both attributes as
/// written and Sema decodes the implication. If both are written, the
/// column is primary; if only `@primary` is written, the column is both
/// primary and unique.
///
/// There is no `@default` attribute (grammar §4.1.5): `T.ADD(args...)`
/// takes exactly one argument per column, in order. Default values are
/// domain logic, expressed as an ordinary wrapper function.
struct ColumnDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ColumnDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    TypeAST* type = nullptr;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// Position of this column in the table's shape, starting at 0.
    /// Assigned by Sema during table resolution; the parser leaves it
    /// at its default (0) and Sema overwrites.
    size_t columnIndex = 0;

    /// True if the column has the `@unique` attribute (or `@primary`,
    /// which implies it). Set by Sema.
    bool isUnique = false;

    /// True if the column has the `@primary` attribute. At most one
    /// column per table has this set; Sema enforces the rule.
    bool isPrimary = false;

    /// True if the column has the `@readonly` attribute. Set by Sema.
    bool isReadonly = false;

    ColumnDeclAST(InternedString n, TypeAST* t)
        : DeclAST(ASTKind::ColumnDecl, n)
        , type(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TableDeclAST
// ─────────────────────────────────────────────────────────────────────────────

// ── RowAST — one inline row inside a table initializer ───────────────────────

/// @brief One row of a table's inline `= [ ... ]` initializer.
///
/// A row is `{ expr, expr, ... }` — one expression per column, in
/// column order. The number of expressions must match the number of
/// columns; Sema checks.
///
/// The initializer may appear on either table kind:
///
///   - On a `@fixed` or `@readonly` TABLE, the row list is the table's entire row set.
///   - On a plain `TABLE`, the row list seeds the growing table; more
///     rows can be added with `T.ADD(...)` at runtime.
///
/// In both cases the cells must be constant expressions (§4.1.1a):
/// literals, arithmetic/unary operations on literals, `T.Member`
/// references to other readonly tables, and — for a function-typed column
/// (§5.0) — a bare top-level `FN` name. The parser produces ordinary
/// `ExprAST*` nodes; Sema validates that each is a `const_expr`.
///
/// `RowAST` is not a `BaseAST` subclass. It is a small struct that
/// `TableDeclAST` holds in its `rows` span. It has no `kind` field and
/// no `loc`; a diagnostic that needs to point at a row points at its
/// first expression's location.
struct RowAST {
    ArenaSpan<ExprAST*> cells;
};

/// @brief A table declaration: `TABLE X { ... }`
///
/// A table is a named, global container of rows. Its shape (columns, in
/// source order) is fixed at declaration. Its rows are either growing
/// (added via `T.ADD(...)` at runtime) or fixed (decided at declaration).
///
/// ─── The two declaration shapes ───────────────────────────────────────────
///
/// A **columned table** has a body of columns and optionally an inline
/// row list:
///
///     TABLE Person {
///         name: string
///         age:  int
///     }
///
/// A **host-backed table** has no columns; it names an opaque native
/// type:
///
///     @export
///     TABLE SpriteRef = host("SpriteRef")
///
/// The two are distinguished by `isHostBacked`. A host-backed table has
/// an empty `columns` span and a non-empty `hostName`.
///
/// ─── Attributes ───────────────────────────────────────────────────────────
///
/// The table's attributes are in the `attributes` span (from `DeclAST`).
/// Sema decodes them into the fields below. The fields are a cache of
/// the span's interpretation, not a second source of truth.
///
/// @field columns         The table's columns, in source order. Empty for a
///                        host-backed table.
/// @field rows            Inline `= [ ... ]` rows, if any. on a `@fixed` or `@readonly` 
//                         table it is the row set, on a plain table it is a seed.
/// @field hostName        The name inside `= host("name")`, if host-backed.
/// @field isHostBacked    True if this is a host-backed table.
/// @field isFixed         True if `@fixed` is present.
/// @field isReadonly      True if `@readonly` is present.
/// @field isPacked        True if `@packed` is present.
/// @field isRequest       True if `@request` is present.
struct TableDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TableDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    /// The table's columns, in source order. Empty for a host-backed
    /// table.
    ArenaSpan<ColumnDeclAST*> columns;

    /// Inline `= [ ... ]` rows, if any.
    ///
    /// Present on either table kind:
    ///   - On a `@fixed` or `@readonly` table, these rows are the table's row set.
    ///   - On a plain table, these rows seed the growing table.
    ///
    /// Each `RowAST*` holds one `ExprAST*` per column, in order. The
    /// cells are restricted to constant expressions (§4.1.1a); the
    /// parser produces them as ordinary `ExprAST*` nodes and Sema
    /// validates.
    ArenaSpan<RowAST*> rows;

    /// True if this is a host-backed table (`TABLE X = host("name")`).
    bool isHostBacked = false;

    /// The name inside `= host("name")`, if host-backed. Invalid
    /// otherwise.
    InternedString hostName;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────

    /// True if the table carries `@fixed` (row set decided at declaration,
    /// cells writable). Distinct from `isReadonly`, which also freezes cells.
    bool isFixed = false;

    /// True if the table carries `@readonly` (row set decided at
    /// declaration, no cell writes). A `@readonly` table is inherently a
    /// fixed-row-set table — `isFixedRowSet()` reports true for either.
    bool isReadonly = false;

    /// True if the table carries `@packed`. Requires `@fixed` or `@readonly`
    /// on the same declaration; Sema checks that requirement.
    bool isPacked = false;

    /// True if the table carries `@reserve(N)`. The value is in
    /// `reservedCount`.
    bool isReserved = false;

    /// N from `@reserve(N)`. Meaningful only when `isReserved` is true.
    /// Non-negative; `@reserve(0)` means "no reservation" but the attribute
    /// is still present.
    uint64_t reservedCount = 0;

    /// True if the table carries `@columnar`.
    bool isColumnar = false;

    /// True if the table carries `@request`. Only valid on a host-backed
    /// table; Sema checks.
    bool isRequest = false;

    // ─── Derived queries ────────────────────────────────────────────────

    /// True if the row set is fixed at declaration. `@fixed` and `@readonly`
    /// both make the row set fixed; `@readonly` additionally freezes cells.
    /// This is what `ADD`/`REMOVE`/`CLEAR`/`SHRINK` availability keys on.
    bool hasFixedRowSet() const { return isFixed || isReadonly; }

    /// True if cells cannot be written. Only `@readonly` implies this.
    bool hasFrozenCells() const { return isReadonly; }

    TableDeclAST(InternedString n) : TypeDeclAST(ASTKind::TableDecl, n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ParamAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One parameter of a `FN` declaration or a lambda.
///
/// A parameter is `[const] name: type`, with an optional `...` before the
/// type for a variadic parameter.
///
/// @example
///   FN f(a: int, b: string) { ... }         → two parameters
///   FN g(const p: &Person) { ... }          → const parameter
///   FN h(nums: ...int) -> int { ... }       → variadic parameter
///
/// A variadic parameter must be the last in the list. The parser
/// produces a `ParamAST` with `isVariadic = true`; Sema enforces the
/// "must be last" rule.
///
/// A `const` parameter marks a read-only binding: the function cannot
/// reassign the parameter, and (if it holds a row reference) cannot
/// mutate the row through it. At the call site, a `const`-bound argument
/// cannot be passed to a non-`const` parameter; Sema checks this.
///
/// The declared type of a variadic parameter is `[T]` — a dynamic array.
/// The grammar writes `...int`; the AST stores the element type `int`
/// and sets `isVariadic`; Sema synthesizes the `[int]` array type.
struct ParamAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const bool isVariadic;

    ParamAST(InternedString n, TypeAST* t, bool isConst, bool isVariadic)
        : ValueDeclAST(ASTKind::Param, n, t, isConst)
        , isVariadic(isVariadic) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FnDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A function declaration.
///
/// The forms:
///
///     FN name(params) -> Ret { body }         — a Lucid-bodied function
///     FN name(params) -> Ret = host("name")   — a host-bound function
///
/// The return type is optional; a function with no `-> T` returns
/// `unit`. A `FN` declaration cannot appear inside another function
/// (grammar §4.2.5); all functions are module-level.
///
/// ─── `@sequence` functions ────────────────────────────────────────────────
/// A function tagged `@sequence` may contain suspension points (`wait`,
/// `waitFrames`, `waitUntil`, `waitForEvent`, `waitForRequest`) in its
/// body and is launched with `start`, not called directly. A sequence
/// always returns `unit` and cannot have a `host(...)` body (grammar
/// §9.2.5).
///
/// ─── Body representation ──────────────────────────────────────────────────
/// A Lucid-bodied function stores its body as a `BlockStmtAST*`. There
/// is no `AnonFuncExprAST` wrapper, no currying chain — the body is one
/// block. A host-bound function has no body (`body == nullptr`) and a
/// non-empty `hostName`.
///
/// ─── Attributes decoded by Sema ───────────────────────────────────────────
/// The `attributes` span (from `DeclAST`) is the source of truth. Sema
/// decodes:
///
///   - `@export`          → `isExported` (on `DeclAST`)
///   - `@deprecated(msg)` → `deprecationMessage`
///   - `@sequence`        → `isSequence`
///
/// @field params        The function's parameters, in order.
/// @field returnType    The declared return type, or null for `unit`.
/// @field body          The body block, or null for a host-bound function.
/// @field hostName      The name inside `= host("name")`, if host-bound.
/// @field isHostBound   True if the body is a `host(...)` target.
/// @field isSequence    True if the function has `@sequence`.
/// @field deprecationMessage The message from `@deprecated(msg)`, if
///                      present. Invalid otherwise.
struct FnDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FnDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<ParamAST*> params;

    /// The declared return type. Null means the function returns `unit`.
    /// (The grammar also allows writing `-> unit` explicitly; the parser
    /// stores the explicit `PrimitiveTypeAST(Unit)` in that case, so a
    /// null `returnType` means "no arrow was written" and a non-null one
    /// with `PrimitiveKind::Unit` means "the arrow was written and named
    /// `unit`." Both are semantically the same.)
    TypeAST* returnType = nullptr;

    /// The body block. Null for a host-bound function.
    BlockStmtAST* body = nullptr;

    /// The name inside `= host("name")`, if host-bound. Invalid
    /// otherwise.
    InternedString hostName;

    /// True if the body is a `host(...)` target.
    bool isHostBound = false;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    bool isSequence = false;

    FnDeclAST(InternedString n)
        : ValueDeclAST(ASTKind::FnDecl, n, /*type=*/nullptr,
                       /*isConst=*/true) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// VarDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A variable declaration: `let x: T = expr` or `const x: T = expr`.
///
/// A variable holds a value: a primitive (copied) or a row/table
/// reference (shared). The binding's mutability is expressed through
/// `ValueDeclAST::isConst`.
///
/// The type annotation is required. The initializer is required. There
/// is no `let x` without a type; the language has no type inference.
///
/// A variable declaration appears at module scope (a top-level
/// declaration) or inside a block (a `VarDeclStmtAST`, see StmtAST.hpp).
/// The AST node is the same in both cases; the wrapper in a block
/// distinguishes the statement form.
///
/// @example
///   let counter: int = 0
///   const PI: float = 3.14159
///   let p: &Person = Person.ADD("alice", 30)
///   const q: &Person = p
struct VarDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* init = nullptr;

    VarDeclAST(InternedString n, TypeAST* t, bool isConst, ExprAST* i)
        : ValueDeclAST(ASTKind::VarDecl, n, t, isConst)
        , init(i) {}
};