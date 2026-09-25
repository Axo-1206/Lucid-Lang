/// @file StmtAST.hpp
/// 
/// @responsibility Defines control flow and action nodes
///                 (blocks, loops, returns, branches, concurrency).
/// 
/// @hierarchy BaseAST → StmtAST → [Concrete Nodes]
/// 
/// @related_files
///   - src/parser/ParserStmt.cpp – primary producer of these nodes
///   - src/semantic/ – consumes for control flow analysis
///
/// ─── Removed Nodes ────────────────────────────────────────────────────────
/// This header deliberately has NO node for:
///   - `AsyncStmtAST` — `async` is a declaration marker on `FuncDeclAST`,
///     not a statement.
///   - `JoinStmtAST` — `await` replaces `join`.
///   - `IntrinsicCallExprAST`'s use for `#scope_exit` — `scope_exit(...)` is
///     an ordinary call registered by Sema; the metadata lives on
///     `BlockStmtAST::scopeExits`, not a dedicated node.

#pragma once

#include "../memory/ArenaSpan.hpp"
#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"

#include <string>
#include <memory>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// ScopeExitRegistration — semantic metadata for scope_exit(...) calls.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A scope-exit callback registration (semantic metadata).
///
/// This is NOT an AST node — it's created by Sema during semantic analysis
/// from a `scope_exit(callback, value)` call. Stored on `BlockStmtAST` as
/// metadata for CodeGen to emit LIFO callbacks on scope exit.
///
/// Multiple registrations within one block run in LIFO order (last
/// registered, first called). The callback must be a function of type
/// `fn(T) -> unit`, where `T` is the type of the value argument, and it
/// must take exactly one argument.
///
/// @field callExpr   The original `scope_exit(...)` call expression (for diagnostics).
/// @field callback   The resolved callback expression — a function value of type `fn(T) -> unit`.
/// @field value      The resolved value expression passed to the callback.
struct ScopeExitRegistration {
    CallExprAST* callExpr = nullptr;   // The original scope_exit(...) call, for diagnostics
    ExprAST*     callback = nullptr;   // Resolved callback expression
    ExprAST*     value    = nullptr;   // Resolved value expression
};
using ScopeExitRegistrationPtr = ScopeExitRegistration*;

// ─────────────────────────────────────────────────────────────────────────────
// BlockStmtAST — the fundamental scoping unit.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A brace‑delimited sequence of statements – the fundamental scoping unit.
/// 
/// @example
///   {
///       let x int = 10
///       io::println(x)
///   }
/// 
/// Every function body, if branch, loop body is a `BlockStmtAST`.
/// The semantic pass opens a new scope when entering a block
/// and closes it on exit – names declared inside are not visible outside.
/// 
/// The block may contain any mix of declarations, control flow statements,
/// expression statements, and nested blocks.
struct BlockStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BlockStmt;

    ArenaSpan<StmtAST*> stmts; // Statements in execution order

    // ─── Scope Exit Registrations (semantic metadata) ───────────────────
    // Each `scope_exit(...)` call in this block is stored here in
    // registration order. LIFO execution: iterate this span in reverse.
    // Populated by Sema during semantic analysis; read by CodeGen.
    ArenaSpan<ScopeExitRegistrationPtr> scopeExits;

    BlockStmtAST() : StmtAST(ASTKind::BlockStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ExprStmtAST — an expression used as a statement.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An expression used as a statement – its value is silently discarded.
/// 
/// @example
///   f(args)                – function call for side effects
///   x |> validate |> save  – pipeline as a statement
///   io::println("done")    – void call
/// 
/// The semantic pass emits a warning when a non‑void expression result is
/// discarded without explicit intent (e.g., a function returning `T!`
/// whose `err` state is never checked).
struct ExprStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ExprStmt;

    ExprAST* expr = nullptr; // The expression being evaluated for its side effects

    explicit ExprStmtAST(ExprAST* e)
        : StmtAST(ASTKind::ExprStmt), expr(e) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// DeclStmtAST — a local declaration inside a block.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A local declaration inside a block body – supports any declaration kind.
/// 
/// @example
///   const compute () -> int = {
///       struct Vec2 { x float = 0.0; y float = 0.0 }
///       const add (a int)(b int) -> int = { ... }
///       enum Color { Red = 0; Green = 1; Blue = 2 }
///       let p Point = Point { x = 5, y = 5 }
///       return add(p.x)(p.y)
///   }
/// 
/// The semantic pass visits the `decl` and registers it in the current block's
/// scope. Types declared locally are only visible within that block.
/// 
/// @note Attributes (`@[inline]`, `@[deprecated]`, ...) are allowed on local
///       declarations. `@[export]` is not allowed inside a block; it is
///       top-level only.
/// @note `import` is top-level only. It never appears inside a `DeclStmtAST`.
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
    bool isDef()     const { return decl && decl->isa<DefDeclAST>(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// IfStmtAST — the statement form of `if`.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The statement form of `if` – `else` is optional, no value is produced.
/// 
/// @example
///   if score >= 90 { io::println("A") }
///   if score >= 90 { io::println("A") } else { io::println("F") }
///   if x < 0 { return } else if x == 0 { ... } else { ... }
/// 
/// Contrast with `IfExprAST` (expression form), which requires `else` and
/// produces a value.
/// 
/// The `elseBranch` can be:
///   - `nullptr`               → no else clause
///   - `BlockStmtAST`          → `else { ... }`
///   - `IfStmtAST`             → `else if ...` (chained)
/// 
/// ─── Condition Evaluation ───────────────────────────────────────────────
/// The condition is evaluated by the truthiness table (see `BinaryExprAST`
/// for the full rule):
///   - A concrete non-nullable, non-fallible type is always true
///     (compile-time fold, with a "condition is always true" warning).
///   - `bool` uses its runtime value.
///   - `T?`, `T!`, `T?!` are runtime checks for the sentinel.
/// 
/// ─── Narrowing ──────────────────────────────────────────────────────────
/// The semantic pass applies narrowing inside `thenBranch`:
///   - `if x != nil { ... }` narrows `x` to non-nullable inside the block.
///   - `if x { ... }` on `T?` is equivalent to `if x != nil { ... }`.
///   - Inverse narrowing applies after a standalone `if` (no `else`) whose
///     body exits: `if x == nil { return }` narrows `x` to non-nullable for
///     the rest of the enclosing scope.
///   - `or`-joined conditions apply their inverses collectively; `and`-joined
///     conditions do not narrow (the compiler cannot tell which conjunct
///     caused the exit).
struct IfStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::IfStmt;

    ExprAST* condition = nullptr;  // Evaluated by the truthiness table
    StmtAST* thenBranch = nullptr; // Always a `BlockStmtAST`
    StmtAST* elseBranch = nullptr; // `nullptr` | `BlockStmtAST` | `IfStmtAST`

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SwitchCaseAST / SwitchStmtAST — value dispatch.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One case clause inside a `switch` statement.
/// 
/// @example
///   case 200, 201, 202: { io::println("success") }
///   case 1..10:         { io::println("light") }
///   case Direction.North, Direction.South: { moveVertical() }
///   case JsonValue.Num(n): { io::println(toStr(n)) }
/// 
/// `values` – one or more `CaseValueAST` entries. Each entry is:
///   - a literal (e.g., `case 200`),
///   - an enum variant (e.g., `case Direction.North`),
///   - a payload-carrying variant with a binding
///     (e.g., `case JsonValue.Num(n)`), or
///   - a literal range (e.g., `case 1..10`).
/// 
/// The body is a block of statements executed when any of the values matches.
/// There is no fallthrough — each case is isolated. Payload bindings
/// introduced by `CaseValueAST` are in scope for the duration of the body
/// block.
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Exhaustiveness**: For enum types, the compiler errors on missing
///    variants when no `default` clause is present.
/// 2. **Range bounds**: Range bounds in case values must be compile-time
///    literals.
/// 3. **Duplicate values**: Duplicate case values within the same switch
///    are a compile error.
/// 4. **Type compatibility**: All case values must be compatible with the
///    switch subject's type.
/// 5. **Payload bindings**: A payload binding (`case Variant(x)`) introduces
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
///       case 200, 201: { io::println("ok") }
///       case 400:      { io::println("bad request") }
///       default:       { io::println("unknown") }
///   }
/// 
///   switch dir {
///       case Direction.North, Direction.South: { moveVertical() }
///       case Direction.East,  Direction.West:  { moveHorizontal() }
///   }
/// 
/// ─── Key Characteristics ────────────────────────────────────────────────
/// - Statement, not expression (produces no value).
/// - `default` clause is optional.
/// - O(1) dispatch via jump table where possible (integer and enum types).
/// - No fallthrough — each case is independent.
/// - Exhaustiveness checking for enum types when `default` is absent.
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Exhaustiveness**: If the subject is an enum type and no `default`
///    clause is present, the compiler errors on missing variants.
/// 2. **Jump table eligibility**: The compiler emits a jump table for
///    integer and enum subjects, guaranteeing O(1) dispatch.
/// 3. **Type compatibility**: The subject's type must be integer, bool, char,
///    string, or enum. Structs, arrays, floats, and function types are
///    rejected.
/// 4. **Default location**: `defaultLoc` is used for error reporting when
///    `defaultBody` is present.
struct SwitchStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchStmt;

    ExprAST* subject = nullptr;                  ///< The value being dispatched
    ArenaSpan<SwitchCaseAST*> cases;             ///< Non‑default case clauses
    BlockStmtAST* defaultBody = nullptr;         ///< `nullptr` if no `default`
    std::optional<SourceLocation> defaultLoc;    ///< Location of `default` keyword

    SwitchStmtAST() : StmtAST(ASTKind::SwitchStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ForStmtAST — range and collection iteration.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Iterates over a collection or a numeric range.
/// 
/// Two forms map to a single node:
/// 
/// ─── Range iteration ────────────────────────────────────────────────────
///   for i int in 0..10        -- inclusive
///   for i int in 0..<10       -- exclusive
///   for i int in 0..10..2     -- step of 2
/// 
/// A single binding (`indexVar`), no value binding. The binding's type must
/// match the range's element type. The optional `step` (a third `..` clause)
/// defaults to `1`. A step of zero is a compile error; a negative step
/// counts down. Step is range-only: collections iterate step 1.
/// 
/// ─── Collection iteration ───────────────────────────────────────────────
///   for i uint, x T in xs     -- array or slice: index + value
///   for k K, v V in m         -- map: key + value
/// 
/// Two bindings. The first binding's type depends on the collection:
///   - Arrays / slices: `uint` (the index).
///   - Maps: `K` (the key type).
/// The second binding is the element type `T` or the map value type `V`.
/// 
/// Use `_` to discard either binding:
///   for _, x int in xs        -- values only
///   for i uint, _ in xs       -- indices only
///   for k string, _ in m      -- keys only
/// 
/// The two-binding form is required for collections. A single binding on a
/// collection is a compile error; use `for _, x T in xs` to iterate values.
/// 
/// ─── Read-only bindings ─────────────────────────────────────────────────
/// Loop bindings are `const` within the body. Assigning to `i` or `v` is a
/// compile error; shadow with a `let` if a mutable copy is needed.
/// 
/// ─── Ignored values (`_`) ───────────────────────────────────────────────
/// The `_` binding requires no type annotation. Attempting to access `_` in
/// the loop body is a compile error.
/// 
/// @field indexVar   The loop's first binding (range value, array index, or
///                   map key). `nullptr` if discarded (`_`).
/// @field valueVar   The loop's second binding (collection element or map
///                   value). `nullptr` for range loops and for `_`.
/// @field iterable   The iterable expression — a `RangeExprAST` for range
///                   iteration, or any collection expression.
/// @field step       Optional step (range loops only, `nullptr` if omitted).
/// @field body       Always a `BlockStmtAST`.
struct ForStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ForStmt;

    ParamAST* indexVar = nullptr;   // First binding, nullptr if discarded
    ParamAST* valueVar = nullptr;   // Second binding, nullptr if discarded or range loop
    ExprAST*  iterable = nullptr;   // Collection or `RangeExprAST`
    ExprAST*  step = nullptr;       // Optional step (range loops only)
    StmtAST*  body = nullptr;       // Always a `BlockStmtAST`

    ForStmtAST() : StmtAST(ASTKind::ForStmt) {}

    /// True if the iterable is a range (`RangeExprAST`); false for a collection.
    bool isRangeIteration() const {
        return iterable && iterable->isa<RangeExprAST>();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// WhileStmtAST / DoWhileStmtAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Condition‑first loop – condition is tested before each iteration.
/// 
/// @example
///   while n < 5 { n += 1 }
///   while queue.notEmpty() { process(queue.pop() ?? defaultItem) }
/// 
/// The loop exits when the condition evaluates to `false` or when a `break`
/// is reached. The condition is evaluated by the truthiness table (see
/// `BinaryExprAST`).
struct WhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WhileStmt;

    ExprAST* condition = nullptr; // Evaluated by the truthiness table
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
/// The condition is evaluated by the truthiness table.
struct DoWhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DoWhileStmt;

    StmtAST* body = nullptr;       ///< Executed at least once (always `BlockStmtAST`)
    ExprAST* condition = nullptr;  ///< Evaluated after each iteration

    DoWhileStmtAST() : StmtAST(ASTKind::DoWhileStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Jump statements
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Exits the enclosing function, optionally yielding a value.
/// 
/// @example
///   return         – void return
///   return 42      – returns a value
///   return a + b   – returns an expression result
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Type matching**: The returned value's type must match the function's
///    declared return type. A bare `return` is only valid in a function whose
///    return type is `unit`.
/// 2. **Fallible propagation**: Returning an un-narrowed fallible value
///    (`T!`) is forbidden — the compiler cannot tell this apart from
///    forgetting to handle the failure. Narrow with `if x == err { return err; }`
///    or `x ?? fallback` first.
/// 3. **Return exhaustiveness**: Every path through a function that returns
///    a non-`unit` value must reach a `return`.
struct ReturnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ReturnStmt;

    ExprAST* value = nullptr; // nullptr for a bare `return`

    ReturnStmtAST() : StmtAST(ASTKind::ReturnStmt) {}
};

/// @brief Exits the nearest enclosing loop (`for`, `while`, `do‑while`).
/// 
/// @example
///   break
/// 
/// @note Only valid directly inside a loop body. Using `break` outside any
///       loop is a semantic error.
struct BreakStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BreakStmt;

    BreakStmtAST() : StmtAST(ASTKind::BreakStmt) {}
};

/// @brief Skips the rest of the current loop iteration and jumps to the next.
/// 
/// @example
///   continue
/// 
/// @note Only valid directly inside a loop body. Using `continue` outside
///       any loop is a semantic error.
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Concurrency statements (await, spawn, start)
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
/// ─── AwaitKind ──────────────────────────────────────────────────────────
/// - `Single`: await one or more deferreds sequentially (`await d` or
///   `await a, b, c`).
/// - `All`: await a group; all must succeed (`await all(a, b, c)`).
/// - `Any`: await a group; the first to complete wins (`await any(a, b, c)`).
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Narrowing**: Each target is narrowed from `Deferred<T>` to `T`
///    after the await.
/// 2. **Cannot await twice**: Once narrowed, re-awaiting is a type error.
/// 3. **`cancel` is mutually exclusive**: `await` after `cancel(d)` is a
///    compile error (a deferred is consumed by exactly one of `await` or
///    `cancel`).
/// 4. **Must be consumed**: A live `Deferred<T>` reaching scope exit without
///    being awaited or cancelled is a compile error.
/// 5. **Scope**: Only valid inside a function body, not at top level.
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

/// @brief A start operation — launches an async call and produces a `Deferred<T>` binding.
/// 
/// @example
///   start d User = fetchUser(7)
/// 
/// `start d T = f(args)` runs `f(args)` asynchronously and binds its
/// `Deferred<T>` handle to `d`. The handle can later be consumed by `await`
/// (to obtain the result) or by `cancel(d)` (to abandon it).
/// 
/// Unlike `spawn`, `start` always produces a binding — the caller is
/// responsible for consuming the deferred on every control-flow path.
/// 
/// ─── Key Characteristics ────────────────────────────────────────────────
/// - Runs on a fiber, scheduled cooperatively by the VM.
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

/// @brief A spawn operation — launches a fire-and-forget fiber.
/// 
/// @example
///   spawn computeHeavyData()
///   spawn logToFile("started")
/// 
/// `spawn f(args)` runs `f(args)` on a new fiber. The result is discarded.
/// Use `start d T = f(args)` when you need to collect the result later.
/// 
/// ─── Key Characteristics ────────────────────────────────────────────────
/// - Runs on a fiber, scheduled cooperatively by the VM.
/// - Fire-and-forget — no handle is produced; the result cannot be collected.
/// - `f` must be an `async` function. Calling a non-`async` function with
///   `spawn` is a compile error.
/// - The spawned function may not capture `Deferred<T>` values (linear-value
///   rule; the same restriction applies to any closure literal).
/// 
/// @field call   The call expression to execute on the new fiber.
struct SpawnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SpawnStmt;

    ExprAST* call = nullptr;   // `spawn f(args);`

    SpawnStmtAST() : StmtAST(ASTKind::SpawnStmt) {}
};