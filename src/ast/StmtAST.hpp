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

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"
#include "ExprAST.hpp"

#include <string>
#include "support/ArenaSpan.hpp"
#include <memory>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// BLOCK
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A brace‑delimited sequence of statements – the fundamental scoping unit.
 *
 * @example
 *   {
 *       let x int = 10
 *       io.printl(x)
 *   }
 *
 * Every function body, if branch, loop body is a
 * BlockStmtAST. The semantic pass opens a new scope when entering a block
 * and closes it on exit – names declared inside are not visible outside.
 *
 * The block may contain any mix of declarations, control flow statements,
 * expression statements, and nested blocks.
 */
struct BlockStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BlockStmt;

    ArenaSpan<StmtAST*> stmts; // Statements in execution order

    BlockStmtAST() : StmtAST(ASTKind::BlockStmt) {}
};
using BlockStmtPtr = BlockStmtAST*;

// ─────────────────────────────────────────────────────────────────────────────
// EXPRESSION & DECLARATION STATEMENTS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An expression used as a statement – its value is silently discarded.
 *
 * @example
 *   f(args)                – function call for side effects
 *   x |> validate |> save  – pipeline as a statement
 *   io.printl("done")      – void call
 *
 * The semantic pass emits a warning when a non‑void expression result is
 * discarded without explicit intent (e.g., a function returning `Result<T>`
 * whose return value is never checked).
 */
struct ExprStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ExprStmt;

    ExprAST* expr; // The expression being evaluated for its side effects

    explicit ExprStmtAST(ExprAST* e)
        : StmtAST(ASTKind::ExprStmt), expr(e) {}

};
using ExprStmtPtr = ExprStmtAST*;

/**
 * @brief A local declaration inside a block body – supports **any** declaration kind.
 *
 * `VarDeclAST` and `FuncDeclAST` can hold any
 * `DeclAST` node (type alias, struct, enum, trait, impl, from, var, func).
 * The parser enforces which declaration kinds are semantically valid inside
 * a block (visibility modifiers and attributes are rejected locally).
 *
 * @example
 *   let compute () -> int = {
 *       type Vec2 = struct { x float, y float }   // local type alias
 *       let add (a int, b int) -> int = { ... }   // local function
 *       struct Point { x int, y int }             // local struct
 *       impl Point { length () -> float = { ... } } // local impl
 *       let p Point = Point { x = 0, y = 0 }
 *       p:length()
 *   }
 *
 * The semantic pass visits the `decl` and registers it in the current block's
 * scope. Types declared locally are only visible within that block.
 */
struct DeclStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DeclStmt;

    DeclAST* decl; // The actual declaration node

    explicit DeclStmtAST(DeclAST* d) : StmtAST(ASTKind::DeclStmt), decl(d) {}

    // Convenience helpers – use decl->isa<T>() directly in most cases
    bool isVar()        const { return decl && decl->isa<VarDeclAST>(); }
    bool isFunc()       const { return decl && decl->isa<FuncDeclAST>(); }
    bool isTypeAlias()  const { return decl && decl->isa<TypeAliasDeclAST>(); }
    bool isStruct()     const { return decl && decl->isa<StructDeclAST>(); }
    bool isImplDecl()   const { return decl && decl->isa<ImplDeclAST>(); }
    bool isFromDecl()   const { return decl && decl->isa<FromDeclAST>(); }
    bool isEnum()       const { return decl && decl->isa<EnumDeclAST>(); }
    bool isUseDecl()    const { return decl && decl->isa<UseDeclAST>(); }
    bool isTrait()      const { return decl && decl->isa<TraitDeclAST>(); }

};
using DeclStmtPtr = DeclStmtAST*;

// ─────────────────────────────────────────────────────────────────────────────
// BRANCHING STATEMENTS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief The statement form of `if` – `else` is optional, no value is produced.
 *
 * @example
 *   if score >= 90 { io.printl("A") }
 *   if score >= 90 { io.printl("A") } else { io.printl("F") }
 *   if x < 0 { return } else if x == 0 { ... } else { ... }
 *
 * Contrast with `IfExprAST` (expression form) which requires `else` and produces a value.
 *
 * The `elseBranch` can be:
 *   - `nullptr`               → no else clause
 *   - `BlockStmtAST`          → `else { ... }`
 *   - `IfStmtAST`             → `else if ...` (chained)
 *
 * The semantic pass applies type narrowing inside `thenBranch` when the condition
 * is an `is` expression (`if x is SomeType { ... }`).
 */
struct IfStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::IfStmt;

    ExprAST* condition;  // The test expression (must resolve to `bool`)
    StmtAST* thenBranch; // Always a `BlockStmtAST`
    StmtAST* elseBranch; // `nullptr` | `BlockStmtAST` | `IfStmtAST`

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}
};
using IfStmtPtr = IfStmtAST*;

/**
 * @brief One case clause inside a `switch` statement.
 *
 * @example
 *   case 200, 201, 202: { io.printl("success") }
 *   case 1..10:         { io.printl("light") }
 *   case 0x41, 0x30..0x39: { handleInput() }
 *
 * `values` – one or more match values. Each entry is:
 *   - a plain `ExprPtr` (single value, e.g., `case 200`)
 *   - a `RangeExprAST` (range, e.g., `case 1..10`)
 *
 * The body is a block of statements executed when any of the values matches.
 * There is no fallthrough – each case is isolated.
 */
struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;

    ArenaSpan<ExprAST*> values;          ///< Match values (literals or ranges)
    BlockStmtAST* body;                 ///< Statements executed on match

    SwitchCaseAST() : BaseAST(ASTKind::SwitchCase) {}
};
using SwitchCasePtr = SwitchCaseAST*;

/**
 * @brief Statement‑oriented value dispatch – runs statement blocks, produces no value.
 *
 * @example
 *   switch code {
 *       case 200, 201: { io.printl("ok") }
 *       case 400:      { io.printl("bad request") }
 *       default:       { io.printl("unknown") }
 *   }
 *
 * Compare with `match` (expression‑oriented):
 *   - `switch` – statement, no return value, `default` optional, no pattern matching
 *   - `match`  – expression, produces a value, `default` required, full pattern matching
 *
 * No fallthrough – each case is independent.
 */
struct SwitchStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchStmt;

    ExprAST* subject;                           ///< The value being dispatched
    ArenaSpan<SwitchCasePtr> cases;             ///< Non‑default case clauses
    BlockStmtAST* defaultBody;                  ///< `nullptr` if no `default`
    std::optional<SourceLocation> defaultLoc;   ///< Location of `default` keyword (for diagnostics)

    SwitchStmtAST() : StmtAST(ASTKind::SwitchStmt) {}
};
using SwitchStmtPtr = SwitchStmtAST*;

// ─────────────────────────────────────────────────────────────────────────────
// LOOP STATEMENTS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Iterates over a collection or an inclusive range.
 *
 * @example
 *   for item in items     { io.printl(item) }
 *   for i    in 0..10     { io.printl(string(i)) }
 *   for v    in mesh.vertices { v.pos = v.pos |> transform }
 *   for i int in 0..10..2 { io.printl(string(i)) }   – step of 2
 *   for i int in arr[1..10] { io.printl(string(i)) }
 *
 * Both grammar forms (collection and range) map to a single node. The semantic
 * pass infers the iteration variable’s type from the iterable:
 *   - If `iterable` is a `RangeExprAST` → variable inferred as `int`
 *   - If `iterable` is a collection    → variable inferred as the element type
 *   - An explicit type annotation (`iterVar->type`) overrides inference.
 *
 * Valid inside the body: `break`, `continue`, `return` (exits the enclosing function).
 */
struct ForStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ForStmt;

    ParamAST* iterVar;  // Iteration variable (name explicit type) – raw pointer
    ExprAST*  iterable; // Collection or `RangeExprAST`
    ExprAST*  step;     // Optional step (only for range loops, `nullptr` if omitted)
    StmtAST*  body;     // Always a `BlockStmtAST`

    ForStmtAST() : StmtAST(ASTKind::ForStmt) {}
};
using ForStmtPtr = ForStmtAST*;

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

    ExprAST* condition; // Must resolve to `bool`
    StmtAST* body;      // Always a `BlockStmtAST`

    WhileStmtAST() : StmtAST(ASTKind::WhileStmt) {}
};
using WhileStmtPtr = WhileStmtAST*;

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

    StmtAST* body;       ///< Executed at least once (always `BlockStmtAST`)
    ExprAST* condition;  ///< Evaluated after each iteration; must resolve to `bool`

    DoWhileStmtAST() : StmtAST(ASTKind::DoWhileStmt) {}
};
using DoWhileStmtPtr = DoWhileStmtAST*;

// ─────────────────────────────────────────────────────────────────────────────
// CONTROL TRANSFER STATEMENTS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Exits the enclosing function, optionally yielding one or more values.
 *
 * @example
 *   return         – void return (no values)
 *   return 42      – returns a single integer
 *   return a + b   – returns an expression result
 *   return x, y    – returns two values
 *
 * The semantic pass checks:
 *   - The number and types of values match the function’s declared return signature.
 *   - A void return (empty `values`) is only valid in void functions.
 *   - `return` is not allowed inside `~parallel` block bodies.
 */
struct ReturnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ReturnStmt;

    ArenaSpan<ExprAST*> values; // Empty for bare `return`, otherwise one or more expressions

    ReturnStmtAST() : StmtAST(ASTKind::ReturnStmt) {}
};
using ReturnStmtPtr = ReturnStmtAST*;

/**
 * @brief Exits the nearest enclosing loop (`for`, `while`, `do‑while`).
 *
 * @example
 *   break
 *
 * Semantic rules:
 *   - Only valid directly inside a loop body.
 *   - Not valid outside any loop (semantic error).
 */
struct BreakStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BreakStmt;

    BreakStmtAST() : StmtAST(ASTKind::BreakStmt) {}
};
using BreakStmtPtr = BreakStmtAST*;

/**
 * @brief Skips the rest of the current loop iteration and jumps to the next.
 *
 * @example
 *   continue
 *
 * Semantic rules:
 *   - Only valid directly inside a loop body.
 *   - Not valid outside any loop (semantic error).
 */
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}
};
using ContinueStmtPtr = ContinueStmtAST*;

// ─────────────────────────────────────────────────────────────────────────────
// MULTI‑ASSIGNMENT STATEMENTS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Multiple variable declaration (with `let` or `const`).
 *
 * Grammar:
 *   multi_assign := decl_keyword var_spec { ',' var_spec } '=' expr
 *   var_spec     := IDENTIFIER type_ann
 *   decl_keyword := 'let' | 'const'
 *
 * @example
 *   let q int, r int = divmod(10, 3)
 *   const w int, h int = getScreenSize()
 *
 * All variables share the same keyword. Each variable has an explicit type.
 * The RHS must be a single expression that returns as many values as there are variables.
 */
struct MultiVarDeclAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::MultiVarDecl;

    DeclKeyword keyword;                                // `let` or `const`
    ArenaSpan<std::pair<InternedString, TypeAST*>> vars; // (name, type) for each variable
    ExprAST* rhs;                                        // Initialiser expression

    MultiVarDeclAST() : StmtAST(ASTKind::MultiVarDecl) {}
};
using MultiVarDeclPtr = MultiVarDeclAST*;

/**
 * @brief Multi‑assignment to existing variables (no `let`/`const`).
 *
 * Grammar:
 *   multi_assign_stmt := expr_lhs { ',' expr_lhs } '=' expr
 *   expr_lhs          := IDENTIFIER | expr '.' IDENTIFIER | expr '[' expr ']'
 *
 * @example
 *   a, b = f()
 *   x.y, arr[i] = g()
 *   p.x, q.y = h()
 *
 * Each left‑hand side must be an assignable lvalue (variable, field access, or index).
 * The RHS must be a single expression producing as many values as there are LHS targets.
 * Values are assigned left to right. Assigning to a `const` is a semantic error.
 * This statement is only allowed inside block scopes (not at top level).
 */
struct MultiAssignStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::MultiAssignStmt;

    ArenaSpan<ExprAST*> lhs; // Left‑hand side lvalues
    ExprAST* rhs;            // Right‑hand side expression (single)

    MultiAssignStmtAST() : StmtAST(ASTKind::MultiAssignStmt) {}
};
using MultiAssignStmtPtr = MultiAssignStmtAST*;