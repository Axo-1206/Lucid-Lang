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
/// This is NOT an AST node - it's created by Sema during semantic analysis
/// from a #scope_exit intrinsic call. Stored on BlockStmtAST as metadata
/// for CodeGen to emit LIFO callbacks on scope exit.
///
/// @field callExpr   The original #scope_exit intrinsic call expression.
/// @field callback   The resolved function to call (FuncDeclAST or closure).
/// @field args       The resolved arguments to pass to the callback.
struct ScopeExitRegistration {
    IntrinsicCallExprAST* callExpr = nullptr;   // The original #scope_exit call
    FuncDeclAST* callback = nullptr;            // Resolved function to call
    ArenaSpan<ExprAST*> args;                   // Resolved arguments
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

/// @brief A statement that references another *named, non-capturing* function.
/// Used for function declarations that delegate directly to a plain function
/// pointer — never a closure.
/// 
/// @example
///   const add (a int)(b int) -> int = math:add
///   const process = module:process
/// 
/// ─── Valid Targets (enforced by the parser, not just Sema) ─────────────────
/// `target` must be an `IdentifierExprAST` or `ModuleAccessExprAST` that
/// resolves to a named `FuncDeclAST`. Per **Function Values and Closures**
/// in the grammar, a named function is always a plain function pointer with
/// no captured state — which is exactly what `resolvedFunction` below is
/// able to hold.
/// 
/// `FieldAccessExprAST` (e.g. `c.getter`) and `CallExprAST` (e.g.
/// `getHandler("double")`) must **not** be routed through this node, even
/// though both are valid `func_body` expressions and both may legally
/// produce a function value. Either can evaluate to a genuine closure — a
/// `{ func, env }` pair — and `resolvedFunction` is a bare `llvm::Function*`
/// with no field to hold an environment pointer. The parser must wrap those
/// two cases in `ReturnStmtAST` instead, whose value flows through ordinary
/// `ExprAST` codegen (`llvmValue` is a generic `llvm::Value*`, capable of
/// holding a full closure aggregate or a pointer to one).
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. This is used when a function declaration has an expression body that
///    is a pure function reference (IdentifierExprAST or ModuleAccessExprAST
///    only — see above).
/// 2. The semantic pass validates that the target resolves to a FuncDeclAST
///    and that the types match.
struct FuncRefStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::FuncRefStmt;
    
    // ─── Parser Fields ──────────────────────────────────────────────────
    ExprAST* target = nullptr;  // IdentifierExprAST or ModuleAccessExprAST only — see above
    
    // ─── CodeGen Annotations ────────────────────────────────────────────
    llvm::Function* resolvedFunction = nullptr;  // The resolved LLVM function —
                                                  // always a plain function pointer,
                                                  // never a closure with an environment

    FuncRefStmtAST() : StmtAST(ASTKind::FuncRefStmt) {}
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

    ExprAST* condition = nullptr;  // The test expression (must resolve to `bool`)
    StmtAST* thenBranch = nullptr; // Always a `BlockStmtAST`
    StmtAST* elseBranch = nullptr; // `nullptr` | `BlockStmtAST` | `IfStmtAST`

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}
};

/// @brief One case clause inside a `switch` statement.
/// 
/// @example
///   case 200, 201, 202: { io:printl("success") }
///   case 1..10:         { io:printl("light") }
///   case 0x41, 0x30..0x39: { handleInput() }
///   case Direction.North, Direction.South: { moveVertical() }
/// 
/// `values` – one or more match values. Each entry is:
///   - a literal (e.g., `case 200`)
///   - an enum variant (e.g., `case Direction.North`)
///   - a literal range (e.g., `case 1..10`)
/// 
/// The body is a block of statements executed when any of the values matches.
/// There is no fallthrough – each case is isolated.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Exhaustiveness**: For enum types, the compiler errors on missing
///    variants when no `default` clause is present.
/// 2. **Range Bounds**: Range bounds in case values must be literals
///    (enforced by the parser).
/// 3. **Duplicate Values**: Duplicate case values within the same switch
///    are a compile error.
/// 4. **Type Compatibility**: All case values must be compatible with the
///    switch subject's type.
struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;

    ArenaSpan<ExprAST*> values;          ///< Match values (literals, enum variants, or ranges)
    BlockStmtAST* body = nullptr;                 ///< Statements executed on match

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
/// collection's own declaration already fixes it. The index is always `int`;
/// the value must match the collection's element type.
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

    ExprAST* condition = nullptr; // Must resolve to `bool`
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
    ExprAST* condition = nullptr;  ///< Evaluated after each iteration; must resolve to `bool`

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
/// 4. **Parallel Body Restriction**: `return` is not allowed inside `~[parallel]`
///    block bodies (no single caller to return to).
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
/// 3. **Parallel Body Restriction**: `break` is not allowed inside `~[parallel]`
///    block bodies (no loop context to break from).
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
/// 3. **Parallel Body Restriction**: `continue` is not allowed inside `~[parallel]`
///    block bodies (no loop context to continue from).
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONCURRENCY STATEMENTS (Async, Spawn, Join)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An async operation – schedules a function call on the event loop.
/// 
/// @example
///   async const result int = fetchData(url)
///   async let result int = fetchData(url)
/// 
/// Note the surface syntax names the *inner* type (`int`), not `Future<T>`
/// directly — the parser wraps it into `Future<int>` itself, the same way
/// `int?` wraps `int` into `NullableTypeAST` without the source ever
/// spelling `Nullable<int>`.
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Cooperative concurrency (single-threaded event loop)
/// - Non-blocking – the calling thread continues immediately
/// - Must be awaited with `await` to get the result
/// - Lightweight – can schedule thousands of async operations
/// - Binds exactly one variable per statement
/// 
/// ─── `binding` Is Always a Fresh Local, Never an Existing Lvalue ──────────
/// `async const result int = ...` *introduces* `result` — it does not assign into
/// a pre-existing variable, the same way `let`/`const` introduce a name
/// rather than reassign one. `binding` is therefore a synthesized
/// `VarDeclAST*`, not a general `ExprAST*` lvalue.
/// 
/// ─── `keyword` Support ─────────────────────────────────────────────────────
/// The `async` statement supports both `let` and `const` keywords:
/// - `async let result int = fn()` → mutable binding (can be awaited, but also reassigned)
/// - `async const result int = fn()` → immutable binding (cannot be reassigned)
/// 
/// The const-ness applies to the binding itself, not to the Future<T> type.
/// 
/// @field binding        The freshly introduced local. `binding->keyword`
///                        is either `Let` or `Const` as specified in source.
/// @field call           The async call expression.
struct AsyncStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::AsyncStmt;

    VarDeclAST* binding = nullptr;   // fresh local introduced by this statement
    ExprAST* call = nullptr;          // the async call

    AsyncStmtAST() : StmtAST(ASTKind::AsyncStmt) {}
};

/// @brief An await operation – waits for async operations to complete.
/// 
/// @example
///   await result
///   await value, ok
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Blocks the current thread until all awaited operations complete
/// - After `await`, the variables become plain `T` (no longer `Future<T>`)
/// - Only valid inside a function body (not at top level)
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Narrowing, Not Reassignment**: Each entry in `targets` is an
///    `IdentifierExprAST` resolving back to the `VarDeclAST` a prior
///    `AsyncStmtAST::binding` introduced. `await` narrows that binding's
///    type from `FutureTypeAST(T)` to plain `T` for the rest of the
///    enclosing scope — the same flow-sensitive mechanism that narrows
///    `T?` after a nil-check, not a distinct runtime state transition.
/// 2. **Cannot Await Twice**: Once narrowed to `T`, the type checker
///    rejects a second `await` on the same binding the same way it rejects
///    any other use of a plain `T` value as if it were still `Future<T>` —
///    there is nothing `Future`-specific to enforce here beyond ordinary
///    type checking once narrowing has already happened.
/// 3. **Multiple Variables**: Waits for all named variables to be ready.
/// 4. **Scope**: Only valid inside a function body (not at top level).
/// 
/// @field targets        The variables to await (must currently be `Future<T>`).
struct AwaitStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::AwaitStmt;

    ArenaSpan<ExprAST*> targets;   // identifiers resolving back to a prior AsyncStmtAST::binding

    AwaitStmtAST() : StmtAST(ASTKind::AwaitStmt) {}
};

/// @brief A spawn operation – launches a function call on a separate OS thread.
/// 
/// @example
///   spawn const result int = computeHeavyData()
///   spawn let result int = computeHeavyData()
///   spawn _ = logToFile("started")            – discard the return value
/// 
/// Note `_` is the one case with no keyword and no type to write at all — the discard
/// pattern never produces a binding, so there is nothing to wrap.
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Parallelism (OS threads)
/// - Preemptive multitasking
/// - Can be joined with `join` to get the result
/// - Heavy overhead – limited to CPU cores (dozens of threads)
/// 
/// ─── The Discard Pattern (`_`) ──────────────────────────────────────────────
/// - `spawn _ = fn()` = fire and forget (`binding == nullptr`, no join required)
/// - `spawn const x T = fn()` = fire and join later (join required)
/// 
/// ─── `keyword` Support ─────────────────────────────────────────────────────
/// The `spawn` statement supports both `let` and `const` keywords for named bindings:
/// - `spawn let result int = fn()` → mutable binding (can be joined and reassigned)
/// - `spawn const result int = fn()` → immutable binding (can be joined but not reassigned)
/// 
/// The const-ness applies to the binding itself, not to the Thread<T> type.
/// 
/// @field binding          The freshly introduced local, or `nullptr` for
///                          the `_` discard pattern.
/// @field call             The spawn call expression.
struct SpawnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SpawnStmt;

    VarDeclAST* binding = nullptr;   // fresh local introduced by this statement, or
                                      // nullptr for the `_` discard pattern
    ExprAST* call = nullptr;          // the spawn call

    SpawnStmtAST() : StmtAST(ASTKind::SpawnStmt) {}
};

/// @brief A join operation – waits for spawned threads to complete.
/// 
/// @example
///   join result
///   join value, ok
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Blocks the current thread until all joined operations complete
/// - After `join`, the variables become plain `T` (no longer `Thread<T>`)
/// - Only valid for `spawn` operations (not `async`)
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Narrowing, Not Reassignment**: Each entry in `targets` is an
///    `IdentifierExprAST` resolving back to the `VarDeclAST` a prior
///    `SpawnStmtAST::binding` introduced. `join` narrows that binding's
///    type from `ThreadTypeAST(T)` to plain `T`, same mechanism as
///    `AwaitStmtAST`.
/// 2. **Cannot Join Twice**: Once narrowed to `T`, a second `join` on the
///    same binding is rejected by ordinary type checking, same as
///    `AwaitStmtAST`.
/// 3. **Multiple Variables**: Waits for all named variables to be ready.
/// 4. **Spawn Only**: `join` only works for `spawn` operations (not `async`).
/// 5. **Discard Pattern**: `_` results are never joined – they are fire-and-forget,
///    and never produce a `ThreadTypeAST` binding to join in the first place.
/// 
/// @field targets        The variables to join (must currently be `Thread<T>`).
struct JoinStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::JoinStmt;

    ArenaSpan<ExprAST*> targets;   // identifiers resolving back to a prior SpawnStmtAST::binding

    JoinStmtAST() : StmtAST(ASTKind::JoinStmt) {}
};