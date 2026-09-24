/// @file StmtAST.hpp
/// 
/// @responsibility Defines control flow and action nodes (Loops, Blocks, Returns).
/// 
/// @hierarchy BaseAST -> StmtAST -> [Concrete Nodes]
/// 
/// @related_files
///   - src/parser/ParserStmt.cpp – primary producer of these nodes
///   - src/semantic/ – consumes for control flow analysis
/// 

#pragma once

#include "../memory/ArenaSpan.hpp"
#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"

#include <string>
#include <memory>
#include <optional>

/// @brief A scope-exit callback registration (semantic metadata).
///
/// This is NOT an AST node — it's created by Sema during semantic analysis
/// from a `scope_exit(callback, value)` call. Stored on `BlockStmtAST` as
/// metadata for CodeGen to emit LIFO callbacks on scope exit.
///
/// Multiple registrations within one block run in LIFO order (last registered,
/// first called). The callback must be `cls`-shaped and take exactly one
/// argument.
///
/// @field callExpr   The original `scope_exit(...)` call expression (for diagnostics).
/// @field callback   The resolved callback expression (a `cls`-shaped function value).
/// @field value      The resolved value expression passed to the callback.
struct ScopeExitRegistration {
    CallExprAST* callExpr = nullptr;   // The original scope_exit(...) call, for diagnostics
    ExprAST*     callback = nullptr;   // Resolved callback expression
    ExprAST*     value    = nullptr;   // Resolved value expression
};
using ScopeExitRegistrationPtr = ScopeExitRegistration*;

/// @brief A brace‑delimited sequence of statements – the fundamental scoping unit.
/// 
/// @example
///   {
///       let x int = 10
///       io:printl(x)
///   }
/// 
/// Every function body, if branch, loop body is a BlockStmtAST.
/// The semantic pass opens a new scope when entering a block
/// and closes it on exit – names declared inside are not visible outside.
/// 
/// The block may contain any mix of declarations, control flow statements,
/// expression statements, and nested blocks.
struct BlockStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BlockStmt;

    ArenaSpan<StmtAST*> stmts; // Statements in execution order

    // ─── Scope Exit Registrations (semantic metadata) ─────────────────────
    // Each #scope_exit call in this block is stored here in registration order.
    // LIFO execution: iterate this span in reverse.
    // Set by Sema during semantic analysis.
    ArenaSpan<ScopeExitRegistrationPtr> scopeExits;


    BlockStmtAST() : StmtAST(ASTKind::BlockStmt) {}
};

/// @brief An expression used as a statement – its value is silently discarded.
/// 
/// @example
///   f(args)                – function call for side effects
///   x |> validate |> save  – pipeline as a statement
///   io:printl("done")      – void call
/// 
/// The semantic pass emits a warning when a non‑void expression result is
/// discarded without explicit intent (e.g., a function returning `T!`
/// whose return value is never checked).
struct ExprStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ExprStmt;

    ExprAST* expr = nullptr; // The expression being evaluated for its side effects

    explicit ExprStmtAST(ExprAST* e)
        : StmtAST(ASTKind::ExprStmt), expr(e) {}
};

/// @brief A local declaration inside a block body – supports any declaration kind.
/// 
/// @example
///   const compute () -> int = {
///       struct Vec2 { x float = 0.0, y float = 0.0 }   // local struct
///       const add (a int)(b int) -> int = { ... }      // local function
///       enum Color { Red = 0, Green = 1, Blue = 2 }    // local enum
///       let p Point = Point { x = 5, y = 5 }
///       return add(p.x)(p.y)
///   }
/// 
/// The semantic pass visits the `decl` and registers it in the current block's
/// scope. Types declared locally are only visible within that block.
/// 
/// @note Attributes (@[inline], @[deprecated]) are allowed on local declarations.
///       @[export] is NOT allowed on local declarations (top-level only).
struct DeclStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DeclStmt;

    DeclAST* decl = nullptr; // The actual declaration node

    explicit DeclStmtAST(DeclAST* d) : StmtAST(ASTKind::DeclStmt), decl(d) {}

    // Convenience helpers – use decl->isa<T>() directly in most cases
    bool isVar()     const { return decl && decl->isa<VarDeclAST>(); }
    bool isFunc()    const { return decl && decl->isa<FuncDeclAST>(); }
    bool isStruct()  const { return decl && decl->isa<StructDeclAST>(); }
    bool isEnum()    const { return decl && decl->isa<EnumDeclAST>(); }
    bool isTrait()   const { return decl && decl->isa<TraitDeclAST>(); }
    bool isUseDecl() const { return decl && decl->isa<ImportDeclAST>(); }
};

/// @brief The statement form of `if` – `else` is optional, no value is produced.
/// 
/// @example
///   if score >= 90 { io:printl("A") }
///   if score >= 90 { io:printl("A") } else { io:printl("F") }
///   if x < 0 { return } else if x == 0 { ... } else { ... }
/// 
/// Contrast with `IfExprAST` (expression form) which requires `else` and produces a value.
/// 
/// The `elseBranch` can be:
///   - `nullptr`               → no else clause
///   - `BlockStmtAST`          → `else { ... }`
///   - `IfStmtAST`             → `else if ...` (chained)
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// The semantic pass applies type narrowing inside branches:
/// 1. **Standard Narrowing**: Inside `thenBranch`, the condition's truth is
///    applied (e.g., `if a != nil { ... }` narrows `a` to non-nullable).
/// 2. **Inverse Narrowing**: For standalone `if` with no `else` that contains
///    a control flow exit (`return`, `break`, `continue`), the inverse of the
///    condition is applied to the rest of the enclosing scope.
/// 3. **`or` at Top Level**: When conditions are joined by `or`, the exit fires
///    if ANY is true. The inverse is ALL negated – every sub-condition's
///    inverse is safely applied.
/// 4. **`and` at Top Level**: Narrowing is unsound and not applied when
///    conditions are joined by `and`.
struct IfStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::IfStmt;

    ExprAST* condition = nullptr;  // The test expression (evaluated by truthiness table)
    StmtAST* thenBranch = nullptr; // Always a `BlockStmtAST`
    StmtAST* elseBranch = nullptr; // `nullptr` | `BlockStmtAST` | `IfStmtAST`

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}
};

/// @brief One case clause inside a `switch` statement.
/// 
/// @example
///   case 200, 201, 202: { printl("success") }
///   case 1..10:         { printl("light") }
///   case Direction.North, Direction.South: { moveVertical() }
///   case JsonValue.Num(n): { printl(n) }
/// 
/// `values` – one or more `CaseValueAST` entries. Each entry is:
///   - a literal (e.g., `case 200`)
///   - an enum variant (e.g., `case Direction.North`)
///   - a payload-carrying variant with binding (e.g., `case JsonValue.Num(n)`)
///   - a literal range (e.g., `case 1..10`)
/// 
/// The body is a block of statements executed when any of the values matches.
/// There is no fallthrough — each case is isolated. Payload bindings introduced
/// by `CaseValueAST` are in scope for the duration of the body block.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Exhaustiveness**: For enum types, the compiler errors on missing
///    variants when no `default` clause is present.
/// 2. **Range Bounds**: Range bounds in case values must be compile-time literals.
/// 3. **Duplicate Values**: Duplicate case values within the same switch
///    are a compile error.
/// 4. **Type Compatibility**: All case values must be compatible with the
///    switch subject's type.
/// 5. **Payload Bindings**: A payload binding (`case Variant(x)`) introduces
///    `x` into the body's scope with the variant's payload type.
struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;

    ArenaSpan<CaseValueAST*> values;  ///< Match values (CaseValueAST entries)
    BlockStmtAST* body = nullptr;     ///< Statements executed on match

    SwitchCaseAST() : BaseAST(ASTKind::SwitchCase) {}
};

/// @brief Statement‑oriented value dispatch – runs statement blocks, produces no value.
/// 
/// @example
///   switch code {
///       case 200, 201: { io:printl("ok") }
///       case 400:      { io:printl("bad request") }
///       default:       { io:printl("unknown") }
///   }
/// 
///   switch dir {
///       case Direction.North, Direction.South: { moveVertical() }
///       case Direction.East,  Direction.West:  { moveHorizontal() }
///   }
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Statement, not expression (produces no value)
/// - `default` clause is optional
/// - O(1) dispatch via jump table where possible (integer and enum types)
/// - No fallthrough – each case is independent
/// - Exhaustiveness checking for enum types when `default` is absent
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Exhaustiveness**: If the subject is an enum type and no `default`
///    clause is present, the compiler errors on missing variants.
/// 2. **Jump Table Eligibility**: The compiler emits a jump table for integer
///    and enum types, guaranteeing O(1) dispatch.
/// 3. **Type Compatibility**: The subject's type must be integer, bool, char,
///    string, or enum. Structs, arrays, floats, and function types are rejected.
/// 4. **Default Location**: `defaultLoc` is used for error reporting when
///    `defaultBody` is present.
struct SwitchStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchStmt;

    ExprAST* subject = nullptr;                  ///< The value being dispatched
    ArenaSpan<SwitchCaseAST*> cases;             ///< Non‑default case clauses
    BlockStmtAST* defaultBody = nullptr;         ///< `nullptr` if no `default`
    std::optional<SourceLocation> defaultLoc;   ///< Location of `default` keyword (for diagnostics)

    SwitchStmtAST() : StmtAST(ASTKind::SwitchStmt) {}
};

/// @brief Iterates over a collection or a numeric range with both index and value.
/// 
/// @example
///   for i int in 0..10  { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) }      -- range inclusive
///   for i int in 0..<10 { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) }      -- range exclusive
///   for i int in 0..10..2 { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) }    -- step of 2
///   for i int, v int in nums { io:printl(stringFromInt(i) + ": " + stringFromInt(v)) } -- collection
///   for _, v int in nums { io:printl(stringFromInt(v)) }                               -- ignore index
///   for i int, _ in nums { io:printl(stringFromInt(i)) }                               -- ignore value
///   for _, _ in nums { io:printl("processing") }                                       -- ignore both
/// 
/// Both grammar forms (range and collection) map to a single node.
/// 
/// ─── Grammar ──────────────────────────────────────────────────────────────
///   for_stmt = 'for' for_binding ',' for_binding 'in' for_iterable [ '..' expr ] block
///   for_binding = IDENTIFIER type | '_'
///   for_iterable = range_iter | expr
///   range_iter = expr range_op expr
///   range_op = '..' | '..<'
/// 
/// ─── Range Iteration ──────────────────────────────────────────────────────
/// Both index and value are required. Use `_` to ignore either.
/// The loop variables' types must be numeric (`int`, `float`, etc.).
/// The end bound's inclusivity is controlled by `range_op` (`..` vs `..<`).
/// An optional trailing `..` *expr* sets the step (defaults to 1).
/// 
/// ─── Collection Iteration ────────────────────────────────────────────────
/// Both index and value are required. Use `_` to ignore either.
/// Every named loop variable requires its own type annotation, even though the
/// collection's own declaration already fixes it. For range iteration: a single binding is used; no index/value split.
/// For collection iteration: the index type is `uint` (for arrays) or the key type `K` (for maps);
/// 
/// ─── Ignored Values (`_`) ──────────────────────────────────────────────────
/// The `_` binding requires no type annotation. Attempting to access `_` in
/// the loop body is a compile error.
/// 
/// @field indexVar      The index variable (name + explicit type) – `nullptr` if ignored (`_`)
/// @field valueVar      The value variable (name + explicit type) – `nullptr` if ignored (`_`)
/// @field iterable      The iterable expression (collection or `RangeExprAST`)
/// @field step          Optional step (only for range loops, `nullptr` if omitted)
/// @field body          Always a `BlockStmtAST`
struct ForStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ForStmt;

    ParamAST* indexVar = nullptr;   // Index variable (name + explicit type), nullptr if ignored (`_`)
    ParamAST* valueVar = nullptr;   // Value variable (name + explicit type), nullptr if ignored (`_`)
    ExprAST*  iterable = nullptr;     // Collection or `RangeExprAST`
    ExprAST*  step = nullptr;         // Optional step (only for range loops, `nullptr` if omitted)
    StmtAST*  body = nullptr;         // Always a `BlockStmtAST`

    ForStmtAST() : StmtAST(ASTKind::ForStmt) {}
};

/// @brief Condition‑first loop – condition is tested before each iteration.
/// 
/// @example
///   while n < 5 { n += 1 }
///   while !queue.isEmpty() { process(queue.pop() ?? defaultItem) }
/// 
/// The loop exits when the condition evaluates to `false` or when a `break` is reached.
struct WhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WhileStmt;

    ExprAST* condition = nullptr; // Evaluated by the truthiness table (see BinaryExprAST)
    StmtAST* body = nullptr;      // Always a `BlockStmtAST`

    WhileStmtAST() : StmtAST(ASTKind::WhileStmt) {}
};

/// @brief Body‑first loop – body executes at least once before condition is checked.
/// 
/// @example
///   do { retries += 1 } while retries < 3
///   do { c = readChar() } while c != '\n'
/// 
/// Useful when the exit condition depends on a side effect of the body.
struct DoWhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DoWhileStmt;

    StmtAST* body = nullptr;       ///< Executed at least once (always `BlockStmtAST`)
    ExprAST* condition = nullptr;  ///< Evaluated after each iteration; uses truthiness table

    DoWhileStmtAST() : StmtAST(ASTKind::DoWhileStmt) {}
};

/// @brief Exits the enclosing function, optionally yielding one or more values.
/// 
/// @example
///   return         – void return (no values)
///   return 42      – returns a single integer
///   return a + b   – returns an expression result
///   return x, y    – returns two values
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Type Matching**: The number and types of values must match the
///    function's declared return signature.
/// 2. **Void Return**: A void return (empty `values`) is only valid in void functions.
/// 3. **Fallible Propagation**: Returning an un-narrowed fallible value is
///    forbidden – the compiler cannot tell this apart from forgetting to
///    handle the failure.
struct ReturnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ReturnStmt;

    ExprAST* value = nullptr; // Empty for bare `return`

    ReturnStmtAST() : StmtAST(ASTKind::ReturnStmt) {}
};

/// @brief Exits the nearest enclosing loop (`for`, `while`, `do‑while`).
/// 
/// @example
///   break
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Loop Context**: Only valid directly inside a loop body.
/// 2. **Not Valid Outside Loop**: Using `break` outside any loop is a semantic error.
struct BreakStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BreakStmt;

    BreakStmtAST() : StmtAST(ASTKind::BreakStmt) {}
};

/// @brief Skips the rest of the current loop iteration and jumps to the next.
/// 
/// @example
///   continue
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Loop Context**: Only valid directly inside a loop body.
/// 2. **Not Valid Outside Loop**: Using `continue` outside any loop is a semantic error.
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONCURRENCY STATEMENTS (Async, Spawn, Join)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An await operation — waits for one or more `Deferred<T>` values to complete.
/// 
/// @example
///   await d
///   await a, b, c
///   await all(a, b, c)
///   await any(a, b, c)
/// 
/// After a successful `await`, each target variable is narrowed from
/// `Deferred<T>` to plain `T` for the rest of the enclosing scope — the same
/// flow-sensitive narrowing mechanism used for `T?`/`T!`.
/// 
/// ─── AwaitKind ──────────────────────────────────────────────────────────────
/// - `Single`: await one or more deferreds sequentially (`await d` or `await a, b, c`).
/// - `All`: await a group; all must succeed (`await all(a, b, c)`).
/// - `Any`: await a group; the first to complete wins (`await any(a, b, c)`).
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Narrowing**: Each target is narrowed from `Deferred<T>` to `T` after await.
/// 2. **Cannot Await Twice**: Once narrowed, re-awaiting is a type error.
/// 3. **`cancel` is mutually exclusive**: `await` after `cancel(d)` is a compile error.
/// 
/// @field kind     The await form (Single, All, Any).
/// @field targets  The `Deferred<T>` identifiers to await.
enum class AwaitKind { Single, All, Any };

struct AwaitStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::AwaitStmt;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    AwaitKind kind = AwaitKind::Single;
    ArenaSpan<ExprAST*> targets;   // identifiers resolving to Deferred<T> bindings

    AwaitStmtAST() : StmtAST(ASTKind::AwaitStmt) {}
};

/// @brief A start operation — launches a call and produces a `Deferred<T>` binding.
/// 
/// @example
///   start result T = fetchData(url)
/// 
/// `start d T = f(args)` runs `f(args)` asynchronously and binds its
/// `Deferred<T>` handle to `d`. The handle can later be consumed by `await`
/// or `cancel(d)`.
/// 
/// Unlike `spawn`, `start` always produces a binding — the caller is responsible
/// for consuming the deferred on every control-flow path (await or cancel).
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Cooperative or OS-thread concurrency (determined at the call site).
/// - Produces a `Deferred<T>` binding that must be consumed exactly once.
/// - The binding's type is `Deferred<T>` where `T` is the written inner type.
/// 
/// @field binding   The freshly introduced local (type = `Deferred<T>`).
/// @field call      The async call expression.
struct StartStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::StartStmt;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    VarDeclAST* binding = nullptr;   // fresh local of type Deferred<T>
    ExprAST*    call    = nullptr;   // the async call

    StartStmtAST() : StmtAST(ASTKind::StartStmt) {}
};

/// @brief A spawn operation — launches a fire-and-forget call on a separate OS thread.
/// 
/// @example
///   spawn computeHeavyData()
///   spawn logToFile("started")
/// 
/// `spawn f(args)` runs `f(args)` on a new OS thread. The result is discarded.
/// Use `start d T = f(args)` when you need to collect the result later.
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Preemptive OS thread — runs in parallel with the caller.
/// - Fire-and-forget — no handle is produced; the result cannot be collected.
/// - The spawned function may not capture `Deferred<T>` values.
/// 
/// @field call   The call expression to execute on the new thread.
struct SpawnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SpawnStmt;

    ExprAST* call = nullptr;   // `spawn f(args);`

    SpawnStmtAST() : StmtAST(ASTKind::SpawnStmt) {}
};