/**
 * @file StmtAST.hpp
 *
 * @responsibility Defines control flow and action nodes (Loops, Blocks, Returns).
 *
 * @hierarchy BaseAST -> StmtAST -> [Concrete Nodes]
 *
 * @related_files
 *   - src/parser/ParserStmt.cpp – primary producer of these nodes
 *   - src/semantic/ – consumes for control flow analysis
 *
 */

#pragma once

#include "../memory/ArenaSpan.hpp"
#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"
#include "ExprAST.hpp"

#include <string>
#include <memory>
#include <optional>

/**
 * @brief A brace‑delimited sequence of statements – the fundamental scoping unit.
 *
 * @example
 *   {
 *       let x int = 10
 *       io:printl(x)
 *   }
 *
 * Every function body, if branch, loop body is a BlockStmtAST.
 * The semantic pass opens a new scope when entering a block
 * and closes it on exit – names declared inside are not visible outside.
 *
 * The block may contain any mix of declarations, control flow statements,
 * expression statements, and nested blocks.
 */
struct BlockStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BlockStmt;

    ArenaSpan<StmtPtr> stmts; // Statements in execution order

    BlockStmtAST() : StmtAST(ASTKind::BlockStmt) {}
};

/**
 * @brief An expression used as a statement – its value is silently discarded.
 *
 * @example
 *   f(args)                – function call for side effects
 *   x |> validate |> save  – pipeline as a statement
 *   io:printl("done")      – void call
 *
 * The semantic pass emits a warning when a non‑void expression result is
 * discarded without explicit intent (e.g., a function returning `T!`
 * whose return value is never checked).
 */
struct ExprStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ExprStmt;

    ExprPtr expr; // The expression being evaluated for its side effects

    explicit ExprStmtAST(ExprPtr e)
        : StmtAST(ASTKind::ExprStmt), expr(e) {}
};

/**
 * @brief A local declaration inside a block body – supports any declaration kind.
 *
 * @example
 *   const compute () -> int = {
 *       struct Vec2 { x float = 0.0, y float = 0.0 }   // local struct
 *       const add (a int)(b int) -> int = { ... }      // local function
 *       enum Color { Red = 0, Green = 1, Blue = 2 }    // local enum
 *       let p Point = Point { x = 5, y = 5 }
 *       return add(p.x)(p.y)
 *   }
 *
 * The semantic pass visits the `decl` and registers it in the current block's
 * scope. Types declared locally are only visible within that block.
 *
 * @note Attributes (@[inline], @[deprecated]) are allowed on local declarations.
 *       @[export] is NOT allowed on local declarations (top-level only).
 */
struct DeclStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DeclStmt;

    DeclPtr decl; // The actual declaration node

    explicit DeclStmtAST(DeclPtr d) : StmtAST(ASTKind::DeclStmt), decl(d) {}

    // Convenience helpers – use decl->isa<T>() directly in most cases
    bool isVar()     const { return decl && decl->isa<VarDeclAST>(); }
    bool isFunc()    const { return decl && decl->isa<FuncDeclAST>(); }
    bool isStruct()  const { return decl && decl->isa<StructDeclAST>(); }
    bool isEnum()    const { return decl && decl->isa<EnumDeclAST>(); }
    bool isTrait()   const { return decl && decl->isa<TraitDeclAST>(); }
    bool isUseDecl() const { return decl && decl->isa<ImportDeclAST>(); }
};

/**
 * @brief Multiple variable declaration (with `let` or `const`).
 *
 * Grammar:
 *   multi_var_decl := ( 'let' | 'const' ) IDENTIFIER type { ',' IDENTIFIER type } '=' expr
 *
 * @example
 *   let value int, ok bool = parseInt("42")
 *   const w int, h int = getScreenSize()
 *
 * All variables share the same keyword. Each variable has an explicit type.
 * The RHS must be a single expression that returns as many values as there are variables.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Count Matching**: The number of variables must match the number of
 *    values returned by the RHS expression.
 * 2. **Type Matching**: Each variable's type must match the corresponding
 *    return value type.
 * 3. **Scope**: Variables are declared in the current scope and visible
 *    after the declaration.
 * 4. **Const Initialization**: If `keyword` is `Const`, all variables must
 *    have initializers (enforced by the parser/semantic pass).
 */
struct MultiVarDeclAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::MultiVarDecl;

    DeclKeyword keyword;                                // `let` or `const`
    ArenaSpan<std::pair<InternedString, TypePtr>> vars; // (name, type) for each variable
    ExprPtr rhs;                                        // Initialiser expression

    MultiVarDeclAST() : StmtAST(ASTKind::MultiVarDecl) {}
};

/**
 * @brief Multi‑assignment to existing variables (no `let`/`const`).
 *
 * Grammar:
 *   multi_assign_stmt := expr_lhs { ',' expr_lhs } '=' expr
 *   expr_lhs          := IDENTIFIER | expr '.' IDENTIFIER | expr '[' expr ']'
 *
 * @example
 *   value, ok = parseInt("42")
 *   x.y, arr[i] = getValues()
 *   p.x, q.y = getPositions()
 *
 * Each left‑hand side must be an assignable lvalue (variable, field access, or index).
 * The RHS must be a single expression producing as many values as there are LHS targets.
 * Values are assigned left to right.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Lvalue Validation**: Each LHS must be an assignable lvalue (variable,
 *    field access, or index expression).
 * 2. **Const Checking**: Assigning to a `const` variable or `const` field is
 *    a semantic error.
 * 3. **Count Matching**: The number of LHS targets must match the number of
 *    values returned by the RHS expression.
 * 4. **Type Matching**: Each LHS target's type must match the corresponding
 *    return value type.
 * 5. **Block Scope Only**: This statement is only allowed inside block scopes
 *    (not at top level).
 */
struct MultiAssignStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::MultiAssignStmt;

    ArenaSpan<ExprPtr> lhs; // Left‑hand side lvalues
    ExprPtr rhs;            // Right‑hand side expression (single)

    MultiAssignStmtAST() : StmtAST(ASTKind::MultiAssignStmt) {}
};

/**
 * @brief The statement form of `if` – `else` is optional, no value is produced.
 *
 * @example
 *   if score >= 90 { io:printl("A") }
 *   if score >= 90 { io:printl("A") } else { io:printl("F") }
 *   if x < 0 { return } else if x == 0 { ... } else { ... }
 *
 * Contrast with `IfExprAST` (expression form) which requires `else` and produces a value.
 *
 * The `elseBranch` can be:
 *   - `nullptr`               → no else clause
 *   - `BlockStmtAST`          → `else { ... }`
 *   - `IfStmtAST`             → `else if ...` (chained)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * The semantic pass applies type narrowing inside branches:
 * 1. **Standard Narrowing**: Inside `thenBranch`, the condition's truth is
 *    applied (e.g., `if a != nil { ... }` narrows `a` to non-nullable).
 * 2. **Inverse Narrowing**: For standalone `if` with no `else` that contains
 *    a control flow exit (`return`, `break`, `continue`), the inverse of the
 *    condition is applied to the rest of the enclosing scope.
 * 3. **`or` at Top Level**: When conditions are joined by `or`, the exit fires
 *    if ANY is true. The inverse is ALL negated – every sub-condition's
 *    inverse is safely applied.
 * 4. **`and` at Top Level**: Narrowing is unsound and not applied when
 *    conditions are joined by `and`.
 */
struct IfStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::IfStmt;

    ExprPtr condition;  // The test expression (must resolve to `bool`)
    StmtPtr thenBranch; // Always a `BlockStmtAST`
    StmtPtr elseBranch; // `nullptr` | `BlockStmtAST` | `IfStmtAST`

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}
};

/**
 * @brief One case clause inside a `switch` statement.
 *
 * @example
 *   case 200, 201, 202: { io:printl("success") }
 *   case 1..10:         { io:printl("light") }
 *   case 0x41, 0x30..0x39: { handleInput() }
 *   case Direction.North, Direction.South: { moveVertical() }
 *
 * `values` – one or more match values. Each entry is:
 *   - a literal (e.g., `case 200`)
 *   - an enum variant (e.g., `case Direction.North`)
 *   - a literal range (e.g., `case 1..10`)
 *
 * The body is a block of statements executed when any of the values matches.
 * There is no fallthrough – each case is isolated.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Exhaustiveness**: For enum types, the compiler errors on missing
 *    variants when no `default` clause is present.
 * 2. **Range Bounds**: Range bounds in case values must be literals
 *    (enforced by the parser).
 * 3. **Duplicate Values**: Duplicate case values within the same switch
 *    are a compile error.
 * 4. **Type Compatibility**: All case values must be compatible with the
 *    switch subject's type.
 */
struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;

    ArenaSpan<ExprPtr> values;          ///< Match values (literals, enum variants, or ranges)
    BlockStmtAST* body;                 ///< Statements executed on match

    SwitchCaseAST() : BaseAST(ASTKind::SwitchCase) {}
};
using SwitchCasePtr = SwitchCaseAST*;

/**
 * @brief Statement‑oriented value dispatch – runs statement blocks, produces no value.
 *
 * @example
 *   switch code {
 *       case 200, 201: { io:printl("ok") }
 *       case 400:      { io:printl("bad request") }
 *       default:       { io:printl("unknown") }
 *   }
 *
 *   switch dir {
 *       case Direction.North, Direction.South: { moveVertical() }
 *       case Direction.East,  Direction.West:  { moveHorizontal() }
 *   }
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - Statement, not expression (produces no value)
 * - `default` clause is optional
 * - O(1) dispatch via jump table where possible (integer and enum types)
 * - No fallthrough – each case is independent
 * - Exhaustiveness checking for enum types when `default` is absent
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Exhaustiveness**: If the subject is an enum type and no `default`
 *    clause is present, the compiler errors on missing variants.
 * 2. **Jump Table Eligibility**: The compiler emits a jump table for integer
 *    and enum types, guaranteeing O(1) dispatch.
 * 3. **Type Compatibility**: The subject's type must be integer, bool, char,
 *    string, or enum. Structs, arrays, floats, and function types are rejected.
 * 4. **Default Location**: `defaultLoc` is used for error reporting when
 *    `defaultBody` is present.
 */
struct SwitchStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchStmt;

    ExprPtr subject;                           ///< The value being dispatched
    ArenaSpan<SwitchCasePtr> cases;             ///< Non‑default case clauses
    BlockStmtAST* defaultBody;                  ///< `nullptr` if no `default`
    std::optional<SourceLocation> defaultLoc;   ///< Location of `default` keyword (for diagnostics)

    SwitchStmtAST() : StmtAST(ASTKind::SwitchStmt) {}
};

/**
 * @brief Iterates over a collection or a numeric range with both index and value.
 *
 * @example
 *   for i int in 0..10  { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) }      -- range inclusive
 *   for i int in 0..<10 { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) }      -- range exclusive
 *   for i int in 0..10..2 { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) }    -- step of 2
 *   for i int, v int in nums { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) } -- collection
 *   for _, v int in nums { io:printl(stringFromInt(v)) }                               -- ignore index
 *   for i int, _ in nums { io:printl(stringFromInt(i)) }                               -- ignore value
 *   for _, _ in nums { io:printl("processing") }                                       -- ignore both
 *
 * Both grammar forms (range and collection) map to a single node.
 *
 * ─── Grammar ──────────────────────────────────────────────────────────────
 *   for_stmt = 'for' for_binding ',' for_binding 'in' for_iterable [ '..' expr ] block
 *   for_binding = IDENTIFIER type | '_'
 *   for_iterable = range_iter | expr
 *   range_iter = expr range_op expr
 *   range_op = '..' | '..<'
 *
 * ─── Range Iteration ──────────────────────────────────────────────────────
 * Both index and value are required. Use `_` to ignore either.
 * The loop variables' types must be numeric (`int`, `float`, etc.).
 * The end bound's inclusivity is controlled by `range_op` (`..` vs `..<`).
 * An optional trailing `..` *expr* sets the step (defaults to 1).
 *
 * ─── Collection Iteration ────────────────────────────────────────────────
 * Both index and value are required. Use `_` to ignore either.
 * Every named loop variable requires its own type annotation, even though the
 * collection's own declaration already fixes it. The index is always `int`;
 * the value must match the collection's element type.
 *
 * ─── Ignored Values (`_`) ──────────────────────────────────────────────────
 * The `_` binding requires no type annotation. Attempting to access `_` in
 * the loop body is a compile error.
 *
 * @field indexVar      The index variable (name + explicit type) – `nullptr` if ignored (`_`)
 * @field valueVar      The value variable (name + explicit type) – `nullptr` if ignored (`_`)
 * @field iterable      The iterable expression (collection or `RangeExprAST`)
 * @field step          Optional step (only for range loops, `nullptr` if omitted)
 * @field body          Always a `BlockStmtAST`
 */
struct ForStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ForStmt;

    ParamAST* indexVar = nullptr;   // Index variable (name + explicit type), nullptr if ignored (`_`)
    ParamAST* valueVar = nullptr;   // Value variable (name + explicit type), nullptr if ignored (`_`)
    ExprPtr  iterable;              // Collection or `RangeExprAST`
    ExprPtr  step;                  // Optional step (only for range loops, `nullptr` if omitted)
    StmtPtr  body;                  // Always a `BlockStmtAST`

    ForStmtAST() : StmtAST(ASTKind::ForStmt) {}
};

/**
 * @brief Condition‑first loop – condition is tested before each iteration.
 *
 * @example
 *   while n < 5 { n += 1 }
 *   while !queue.isEmpty() { process(queue.pop() ?? defaultItem) }
 *
 * The loop exits when the condition evaluates to `false` or when a `break` is reached.
 */
struct WhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WhileStmt;

    ExprPtr condition; // Must resolve to `bool`
    StmtPtr body;      // Always a `BlockStmtAST`

    WhileStmtAST() : StmtAST(ASTKind::WhileStmt) {}
};

/**
 * @brief Body‑first loop – body executes at least once before condition is checked.
 *
 * @example
 *   do { retries += 1 } while retries < 3
 *   do { c = readChar() } while c != '\n'
 *
 * Useful when the exit condition depends on a side effect of the body.
 */
struct DoWhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DoWhileStmt;

    StmtPtr body;       ///< Executed at least once (always `BlockStmtAST`)
    ExprPtr condition;  ///< Evaluated after each iteration; must resolve to `bool`

    DoWhileStmtAST() : StmtAST(ASTKind::DoWhileStmt) {}
};

/**
 * @brief Exits the enclosing function, optionally yielding one or more values.
 *
 * @example
 *   return         – void return (no values)
 *   return 42      – returns a single integer
 *   return a + b   – returns an expression result
 *   return x, y    – returns two values
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Type Matching**: The number and types of values must match the
 *    function's declared return signature.
 * 2. **Void Return**: A void return (empty `values`) is only valid in void functions.
 * 3. **Fallible Propagation**: Returning an un-narrowed fallible value is
 *    forbidden – the compiler cannot tell this apart from forgetting to
 *    handle the failure.
 * 4. **Parallel Body Restriction**: `return` is not allowed inside `~[parallel]`
 *    block bodies (no single caller to return to).
 */
struct ReturnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ReturnStmt;

    ArenaSpan<ExprPtr> values; // Empty for bare `return`, otherwise one or more expressions

    ReturnStmtAST() : StmtAST(ASTKind::ReturnStmt) {}
};

/**
 * @brief Exits the nearest enclosing loop (`for`, `while`, `do‑while`).
 *
 * @example
 *   break
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Loop Context**: Only valid directly inside a loop body.
 * 2. **Not Valid Outside Loop**: Using `break` outside any loop is a semantic error.
 * 3. **Parallel Body Restriction**: `break` is not allowed inside `~[parallel]`
 *    block bodies (no loop context to break from).
 */
struct BreakStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BreakStmt;

    BreakStmtAST() : StmtAST(ASTKind::BreakStmt) {}
};

/**
 * @brief Skips the rest of the current loop iteration and jumps to the next.
 *
 * @example
 *   continue
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Loop Context**: Only valid directly inside a loop body.
 * 2. **Not Valid Outside Loop**: Using `continue` outside any loop is a semantic error.
 * 3. **Parallel Body Restriction**: `continue` is not allowed inside `~[parallel]`
 *    block bodies (no loop context to continue from).
 */
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONCURRENCY STATEMENTS (Async, Spawn, Join)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An async operation – schedules a function call on the event loop.
 *
 * @example
 *   async result = fetchData(url)
 *   async value, ok = parseInt("42")
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - Cooperative concurrency (single-threaded event loop)
 * - Non-blocking – the calling thread continues immediately
 * - Must be awaited with `await` to get the result
 * - Lightweight – can schedule thousands of async operations
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Future Type**: The variable's type becomes `Future<T>` after async assignment.
 * 2. **Cannot Use**: A `Future<T>` cannot be used as `T` – it must be awaited first.
 * 3. **Multiple Returns**: One variable per return value of the call expression.
 * 4. **Await Required**: The compiler warns about unawaited async operations
 *    when the scope exits.
 *
 * @field target         The variables being assigned to.
 * @field call           The async call expression.
 */
struct AsyncStmtAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AsyncExpr;

    ArenaSpan<ExprPtr> target;   // variables to bind (must be lvalues)
    ExprPtr call;                // the async call

    AsyncStmtAST() : ExprAST(ASTKind::AsyncExpr) {}
};

/**
 * @brief An await operation – waits for async operations to complete.
 *
 * @example
 *   await result
 *   await value, ok
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - Blocks the current thread until all awaited operations complete
 * - After `await`, the variables become plain `T` (no longer `Future<T>`)
 * - Only valid inside a function body (not at top level)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Future to T**: After `await`, each variable's type changes from
 *    `Future<T>` to `T`.
 * 2. **Cannot Await Twice**: Awaited variables are plain `T` – awaiting again
 *    is a compile error.
 * 3. **Multiple Variables**: Waits for all named variables to be ready.
 * 4. **Scope**: Only valid inside a function body (not at top level).
 *
 * @field targets        The variables to await (must be `Future<T>`).
 */
struct AwaitStmtAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AwaitExpr;

    ArenaSpan<ExprPtr> targets;   // variables to await (must be Future<T>)

    AwaitStmtAST() : ExprAST(ASTKind::AwaitExpr) {}
};

/**
 * @brief A spawn operation – launches a function call on a separate OS thread.
 *
 * @example
 *   spawn result = computeHeavyData()
 *   spawn _, _ = parseAndValidate(data)  – discard all return values
 *   spawn user, _, _ = processUser(data) – keep some, discard others
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - Parallelism (OS threads)
 * - Preemptive multitasking
 * - Can be joined with `join` to get the result
 * - Heavy overhead – limited to CPU cores (dozens of threads)
 *
 * ─── The Discard Pattern (`_`) ──────────────────────────────────────────────
 * - `spawn _ = fn()` = fire and forget (no join required)
 * - `spawn x = fn()` = fire and join later (join required)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Future Type**: The variable's type becomes `Future<T>` after spawn assignment.
 * 2. **Cannot Use**: A `Future<T>` cannot be used as `T` – it must be joined first.
 * 3. **Discard Pattern**: `_` means the result is discarded – no join required.
 * 4. **Multiple Returns**: One variable per return value of the call expression.
 * 5. **Join Warning**: The compiler warns about named spawns that are never joined.
 * 6. **Shared State**: Variables declared before the spawn call are shared
 *    between threads (requires synchronization).
 * 7. **Nesting**: A spawned thread can itself launch further spawn or async calls.
 *
 * @field targets        The variables being assigned to (`_` means discard).
 * @field call           The spawn call expression.
 */
struct SpawnStmtAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::SpawnExpr;

    ArenaSpan<ExprPtr> targets;   // variables to bind (`_` for discard)
    ExprPtr call;                // the spawn call

    SpawnStmtAST() : ExprAST(ASTKind::SpawnExpr) {}
};

/**
 * @brief A join operation – waits for spawned threads to complete.
 *
 * @example
 *   join result
 *   join value, ok
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - Blocks the current thread until all joined operations complete
 * - After `join`, the variables become plain `T` (no longer `Future<T>`)
 * - Only valid for `spawn` operations (not `async`)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Future to T**: After `join`, each variable's type changes from
 *    `Future<T>` to `T`.
 * 2. **Cannot Join Twice**: Joined variables are plain `T` – joining again
 *    is a compile error.
 * 3. **Multiple Variables**: Waits for all named variables to be ready.
 * 4. **Spawn Only**: `join` only works for `spawn` operations (not `async`).
 * 5. **Discard Pattern**: `_` results are never joined – they are fire-and-forget.
 *
 * @field targets        The variables to join (must be `Future<T>` from spawn).
 */
struct JoinStmtAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::JoinExpr;

    ArenaSpan<ExprPtr> targets;   // variables to join (must be Future<T> from spawn)

    JoinStmtAST() : ExprAST(ASTKind::JoinExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Aliases for common pointer types.
// ─────────────────────────────────────────────────────────────────────────────

using BlockStmtPtr = BlockStmtAST*;
using ExprStmtPtr = ExprStmtAST*;
using DeclStmtPtr = DeclStmtAST*;
using IfStmtPtr = IfStmtAST*;
using SwitchCasePtr = SwitchCaseAST*;
using SwitchStmtPtr = SwitchStmtAST*;
using ForStmtPtr = ForStmtAST*;
using WhileStmtPtr = WhileStmtAST*;
using DoWhileStmtPtr = DoWhileStmtAST*;
using ReturnStmtPtr = ReturnStmtAST*;
using BreakStmtPtr = BreakStmtAST*;
using ContinueStmtPtr = ContinueStmtAST*;
using MultiVarDeclPtr = MultiVarDeclAST*;
using MultiAssignStmtPtr = MultiAssignStmtAST*;
using AsyncExprPtr = AsyncStmtAST*;
using AwaitExprPtr = AwaitStmtAST*;
using SpawnExprPtr = SpawnStmtAST*;
using JoinExprPtr = JoinStmtAST*;