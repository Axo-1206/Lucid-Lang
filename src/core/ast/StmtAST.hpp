/**
 * @file StmtAST.hpp
 *
 * @responsibility The AST nodes for statements. Every statement form in
 *                 the grammar is one node here, including the five
 *                 sequence suspend points.
 *
 * @hierarchy BaseAST → StmtAST → [Concrete Statement Nodes]
 *
 * ─── Design: seventeen statement forms ────────────────────────────────────
 * A statement is exactly one of:
 *
 *   - a block                 (`{ ... }`)
 *   - a local variable decl   (`let x: T = expr;`)
 *   - an assignment           (`x = expr;`, `x += expr;`)
 *   - an expression statement (`f(x);`)
 *   - a return                (`return;`, `return expr;`)
 *   - a break                 (`break;`, `break label;`)
 *   - a continue              (`continue;`, `continue label;`)
 *   - an if                   (`if cond { ... } [else ...]`)
 *   - a switch                (`switch expr { case ...: { ... } default: { ... } }`)
 *   - a while                 (`[label:] while cond { ... }`)
 *   - a for                   (`[label:] for x: T in iterable { ... }`)
 *   - wait                    (`wait(seconds);`)
 *   - waitFrames              (`waitFrames(n);`)
 *   - waitUntil               (`waitUntil(pred, arg);`)
 *   - waitForEvent            (`waitForEvent(EventKind.Member);`)
 *   - waitForRequest          (`waitForRequest(req);`)
 *
 * The sequence suspend points (`wait`, `waitFrames`, ...) are the only
 * statements that are syntactically restricted to a specific context: a
 * function tagged `@sequence`. The parser produces them anywhere; Sema
 * checks they appear only in a sequence body.
 *
 * ─── Design: assignments are statements, not expressions ──────────────────
 * The grammar's `assign_stmt` is a statement form; there is no
 * `assign_expr`. An assignment is never nested inside another
 * expression. The AST reflects this: `AssignStmtAST` is a statement,
 * and there is no `AssignExprAST`.
 *
 * ─── Design: labels are fields on the loop, not separate nodes ────────────
 * A `label: while ...` or `label: for ...` records the label as an
 * `InternedString` field on the loop statement. The label is only
 * meaningful to `break`/`continue`, which carry their own optional
 * target label. Labels do not introduce a scope and do not shadow other
 * names; they are a small namespace of their own.
 *
 * ─── Design: switch has a mandatory default ───────────────────────────────
 * The grammar (§12.2) requires `default` in every `switch`. The AST
 * reflects this: `SwitchStmtAST::defaultBody` is always present after
 * a successful parse. If the source omitted it, the parser produces a
 * placeholder block and a syntax error.
 */

#pragma once

#include "BaseAST.hpp"
#include "DeclAST.hpp"
#include "ExprAST.hpp"
#include "TypeAST.hpp"

#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// BlockStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A brace-delimited sequence of statements.
///
/// Every function body, if-branch, loop body, and switch case body is a
/// `BlockStmtAST`. The semantic pass opens a new scope when entering a
/// block and closes it on exit; names declared inside are not visible
/// outside.
///
/// A block's `scope_exits` metadata from the old design is gone. The
/// language has no `scope_exit` construct; the sequence suspend points
/// are the closest thing, and they are handled as statements.
struct BlockStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BlockStmt;

    ArenaSpan<StmtAST*> stmts;

    BlockStmtAST() : StmtAST(ASTKind::BlockStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// VarDeclStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A local variable declaration used as a statement.
///
/// Wraps a `VarDeclAST` — the same node used for a top-level variable
/// declaration. The wrapper distinguishes "this declaration appears in
/// a block, and is therefore a statement" from "this declaration
/// appears at module scope."
///
/// The parser produces this node when it sees `let`/`const` inside a
/// block. The `VarDeclAST` it wraps is constructed the same way as a
/// top-level variable declaration's.
struct VarDeclStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::VarDeclStmt;

    VarDeclAST* decl = nullptr;

    explicit VarDeclStmtAST(VarDeclAST* d)
        : StmtAST(ASTKind::VarDeclStmt), decl(d) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// AssignStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An assignment statement: `lvalue = expr;` or a compound form.
///
/// The left-hand side must be an lvalue: an identifier, a field access
/// on an lvalue, or an index on an lvalue. Sema checks this.
///
/// Compound assignments (`+=`, `-=`, ...) desugar to `lhs = lhs op rhs`
/// at Sema time. The AST stores the compound operator tag; the semantic
/// pass produces the equivalent tree.
///
/// @example
///   x = 5        → op = Assign
///   x += 1       → op = AddAssign
///   row.age = 30 → op = Assign
///   arr[i] = 5   → op = Assign
struct AssignStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::AssignStmt;

    const AssignOp op;
    ExprAST*       lhs = nullptr;
    ExprAST*       rhs = nullptr;

    explicit AssignStmtAST(AssignOp o)
        : StmtAST(ASTKind::AssignStmt), op(o) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ExprStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An expression used as a statement: `f(x);`.
///
/// The expression's result is discarded. The parser produces this node
/// for any statement that is an expression and is not an assignment.
struct ExprStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ExprStmt;

    ExprAST* expr = nullptr;

    explicit ExprStmtAST(ExprAST* e)
        : StmtAST(ASTKind::ExprStmt), expr(e) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ReturnStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A return statement: `return;` or `return expr;`.
///
/// A bare `return;` is legal in a `unit`-returning function. A
/// value-returning function must return a value on every path.
///
/// @field value  The returned expression, or null for a bare return.
struct ReturnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ReturnStmt;

    ExprAST* value = nullptr;

    ReturnStmtAST() : StmtAST(ASTKind::ReturnStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// BreakStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A break statement: `break;` or `break label;`.
///
/// With no label, exits the nearest enclosing loop. With a label, exits
/// the loop whose label matches.
///
/// The label is an `InternedString`; invalid when no label was written.
/// Sema resolves it against the enclosing loops' labels.
struct BreakStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BreakStmt;

    InternedString label;   // invalid when no label

    BreakStmtAST() : StmtAST(ASTKind::BreakStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ContinueStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A continue statement: `continue;` or `continue label;`.
///
/// With no label, jumps to the next iteration of the nearest enclosing
/// loop. With a label, jumps to the next iteration of the loop whose
/// label matches.
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    InternedString label;   // invalid when no label

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// IfStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An if statement: `if cond { ... } [else ...]`.
///
/// The condition is evaluated by the truthiness rules (§6.8 and §5.2):
/// `bool` uses its runtime value; `&T` is a nil check (false if nil); a
/// non-nullable primitive is a compile-time fold to true.
///
/// The `elseBranch` is:
///   - `nullptr`          — no else clause
///   - `BlockStmtAST`     — an else block
///   - `IfStmtAST`        — `else if` (a chained if-statement)
///
/// The AST does not have an if-*expression* form. An if is always a
/// statement; there is no `if cond ?? a else b`.
struct IfStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::IfStmt;

    ExprAST* condition  = nullptr;
    StmtAST* thenBranch = nullptr;   // always a BlockStmtAST
    StmtAST* elseBranch = nullptr;   // nullptr | BlockStmtAST | IfStmtAST

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SwitchCaseAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One `case` clause inside a switch.
///
/// A case matches one or more values, separated by commas in source. The
/// body is a block.
///
/// Each value is a constant expression — typically a fixed-table member
/// reference (`Direction.North`), a literal, a small arithmetic
/// combination of literals, or a range. Sema enforces constant-ness.
///
/// There is no fallthrough, no `break` in the case.
///
/// @field values  The match values. Each is a constant expression or a
///                range. At least one value.
/// @field body    The case body block.
struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;

    ArenaSpan<ExprAST*> values;
    BlockStmtAST*       body = nullptr;

    SwitchCaseAST() : BaseAST(ASTKind::SwitchCase) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SwitchStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A switch statement: `switch expr { case ...: { ... } default: { ... } }`.
///
/// The subject's type determines what `case` expressions are valid. For
/// a subject of type `&T` where `T` is a fixed table, Sema checks the
/// cases against the table's members and emits a **warning** (not an
/// error) if any member is missing. The `default` clause is always
/// present and always reachable, so a missing case is never a
/// correctness bug — just a possible sign the switch needs updating.
///
/// The `default` clause is mandatory. A switch without one is a syntax
/// error.
///
/// @field subject      The value being switched on.
/// @field cases        The case clauses, in source order.
/// @field defaultBody  The default clause's body (always present).
/// @field defaultLoc   The location of the `default` keyword.
struct SwitchStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchStmt;

    ExprAST*                     subject = nullptr;
    ArenaSpan<SwitchCaseAST*>    cases;
    BlockStmtAST*                defaultBody = nullptr;
    SourceLocation               defaultLoc;

    SwitchStmtAST() : StmtAST(ASTKind::SwitchStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// WhileStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A while loop: `[label:] while cond { ... }`.
///
/// The condition is tested before each iteration. The truthiness rules
/// apply, same as for `if`.
///
/// A label, if present, is used by `break label` / `continue label` to
/// target this loop specifically from inside a nested one.
struct WhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WhileStmt;

    InternedString label;   // invalid when no label
    ExprAST*       condition = nullptr;
    BlockStmtAST*  body      = nullptr;

    WhileStmtAST() : StmtAST(ASTKind::WhileStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ForStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A for loop: `[label:] for binding[, binding] in iterable { ... }`.
///
/// The form of the binding list depends on the iterable:
///
///   - **Range.** One binding — the loop counter. The counter's type
///     must match the range's bound type.
///       for i: int in 0..<10 { ... }
///       for i: int in 0..10..2 { ... }        -- step of 2
///
///   - **Table or table view.** One binding — a row reference.
///       for r: &Person in Person { ... }
///
///   - **Column view.** One binding — the column's value type.
///       for v: int in Person.age { ... }
///
///   - **Array.** One or two bindings. One binding iterates the
///     elements; two iterate the index and the element.
///       for x: int in scores { ... }
///       for i: uint, x: int in scores { ... }
///
///   - **Key/value iterable (a Map).** Two bindings — the key and the
///     value.
///       for k: string, v: int in scores { ... }
///
/// A binding may be `_` to discard its value; a discard is represented
/// as a `nullptr` in the corresponding field. The parser produces
/// whichever bindings the source wrote; Sema validates that the shape
/// matches the iterable's type.
///
/// @field label       The loop's optional label.
/// @field firstVar    The first binding, or null for a discard.
/// @field secondVar   The second binding, or null (no second binding
///                    written, or a discard at that position).
/// @field iterable    The iterable expression. May be a RangeExprAST.
/// @field body        The loop body block.
struct ForStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ForStmt;

    InternedString label;      // invalid when no label
    ParamAST*      firstVar  = nullptr;
    ParamAST*      secondVar = nullptr;
    ExprAST*       iterable  = nullptr;
    BlockStmtAST*  body      = nullptr;

    ForStmtAST() : StmtAST(ASTKind::ForStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Sequence suspend points
// ─────────────────────────────────────────────────────────────────────────────
//
// The five suspend-point statements from §9.2. Each is a keyword-led
// statement ending with `;`. They are only valid inside a function
// tagged `@sequence`; the parser produces them anywhere, and Sema
// enforces the placement.

/// @brief `wait(seconds);` — suspend for a real-time duration.
///
/// @field seconds  A float expression giving the duration in seconds.
struct WaitStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WaitStmt;

    ExprAST* seconds = nullptr;

    WaitStmtAST() : StmtAST(ASTKind::WaitStmt) {}
};

/// @brief `waitFrames(n);` — suspend for a number of engine ticks.
///
/// @field frames  A uint expression giving the frame count.
struct WaitFramesStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WaitFramesStmt;

    ExprAST* frames = nullptr;

    WaitFramesStmtAST() : StmtAST(ASTKind::WaitFramesStmt) {}
};

/// @brief `waitUntil(pred, arg);` — suspend until a predicate is true.
///
/// The predicate is a function value of type `(T) -> bool`, and `arg`
/// is a value of type `T`. The predicate is re-evaluated once per tick
/// until it returns true.
///
/// @field predicate  The predicate function.
/// @field arg        The argument passed to the predicate each tick.
struct WaitUntilStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WaitUntilStmt;

    ExprAST* predicate = nullptr;
    ExprAST* arg       = nullptr;

    WaitUntilStmtAST() : StmtAST(ASTKind::WaitUntilStmt) {}
};

/// @brief `waitForEvent(EventKind.Member);` — suspend until an event fires.
///
/// The argument is a fixed-table member reference naming the event kind.
/// The sequence resumes the next time that event kind fires. No polling.
///
/// @field event  The event kind expression (a fixed-table member).
struct WaitForEventStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WaitForEventStmt;

    ExprAST* event = nullptr;

    WaitForEventStmtAST() : StmtAST(ASTKind::WaitForEventStmt) {}
};

/// @brief `waitForRequest(req);` — suspend until a host request completes.
///
/// The argument is a value whose type is an `@request`-attributed host
/// type. The sequence resumes when the host signals that specific
/// request as complete. No polling.
///
/// @field request  The request handle expression.
struct WaitForRequestStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WaitForRequestStmt;

    ExprAST* request = nullptr;

    WaitForRequestStmtAST() : StmtAST(ASTKind::WaitForRequestStmt) {}
};