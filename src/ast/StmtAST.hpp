/**
 * @file StmtAST.hpp
 *
 * @responsibility Defines control flow and action nodes (Loops, Blocks, Returns).
 * 
 * @hierarchy BaseAST -> StmtAST -> [Concrete Nodes]
 *
 * @related_files 
 *   - src/parser/ParserStmt.cpp (The primary producer)
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"
#include "ExprAST.hpp"

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <variant>

// ─────────────────────────────────────────────────────────────────────────────
// StmtAST.hpp — all statement nodes
//
// Every node here inherits from StmtAST (defined in BaseAST.hpp).
// This is the last file in the include chain:
//
//   BaseAST.hpp
//       ↑
//   TypeAST.hpp
//       ↑
//   DeclAST.hpp   (needs TypeAST for param/field types)
//       ↑
//   ExprAST.hpp   (needs TypeAST + DeclAST for params in AnonFuncExprAST)
//       ↑
//   StmtAST.hpp   (needs all of the above)
//
// DeclAST bodies (FuncDeclAST, MethodDeclAST, FromDeclAST) are stored as
// StmtPtr. That forward declaration lives in BaseAST.hpp so DeclAST.hpp can
// use it without including StmtAST.hpp and creating a cycle.
//
// Node inventory:
//   BlockStmtAST         — { stmt* }
//   ExprStmtAST          — expr used as a statement (result discarded)
//   DeclStmtAST          — var_decl or func_decl inside a block
//   IfStmtAST            — if expr block [else (if_stmt | block)]
//   SwitchCaseAST        — case value[, value]* : stmts  (helper, not a StmtAST)
//   SwitchStmtAST        — switch expr { case* default? }
//   ForStmtAST           — for IDENTIFIER in expr/range block
//   WhileStmtAST         — while expr block
//   DoWhileStmtAST       — do block while expr
//   ReturnStmtAST        — return [expr]
//   BreakStmtAST         — break
//   ContinueStmtAST      — continue
//   parallel_for         := 'parallel' 'for' IDENTIFIER [ type ] 'in' ( range_iter | expression ) [ 'step' expression ] block
//   ParallelBlockStmtAST — parallel { block* }
// ─────────────────────────────────────────────────────────────────────────────


// ═════════════════════════════════════════════════════════════════════════════
// BLOCK
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// BlockStmtAST
//
// A brace-delimited sequence of statements — the fundamental scoping unit.
//   { stmt* }
//
// Every function body, if branch, loop body, and parallel sub-block is a
// BlockStmtAST. The semantic pass opens a new scope when entering a block
// and closes it on exit — names declared inside are not visible outside.
//
// stmts may contain any mix of:
//   - DeclStmtAST (var/func declarations)
//   - control flow statements
//   - expression statements
//   - nested blocks
// ─────────────────────────────────────────────────────────────────────────────

struct BlockStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BlockStmt;

    std::vector<StmtPtr> stmts;

    BlockStmtAST() : StmtAST(ASTKind::BlockStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// EXPRESSION & DECLARATION STATEMENTS
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// ExprStmtAST
//
// An expression used as a statement — its value is silently discarded.
//   f(args)                — function call for side effects
//   x -> validate -> save  — pipeline as a statement
//   io.printl("done")      — void call
//
// The semantic pass emits a warning when a non-void expression result is
// discarded without explicit intent — e.g. a function returning Result<T>
// whose return value is never checked (grammar rule from LUC_ERROR.md).
// ─────────────────────────────────────────────────────────────────────────────

struct ExprStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ExprStmt;

    ExprPtr expr;

    explicit ExprStmtAST(ExprPtr e)
        : StmtAST(ASTKind::ExprStmt), expr(std::move(e)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// LocalDecl — the two declaration kinds that are valid inside a block.
//
// Only VarDeclAST and FuncDeclAST may appear inside a block body.
// This variant enforces that structurally — neither pub/export nor extern can
// sneak in because:
//
//   - DeclPtr (the base DeclAST*) is NOT used here. Using the base pointer
//     would allow any declaration kind including StructDeclAST, ImplDeclAST,
//     ExternDeclAST, etc. — none of which are valid inside a block.
//
//   - pub/export is a file-to-package visibility modifier. Inside a block every
//     name is scoped to that block and gone when the block exits. 'pub'/'export'
//     is structurally meaningless and the parser must set isPub = false
//     on any VarDeclAST or FuncDeclAST it constructs for a block context.
//
//   - extern is a linker-level directive. Linkage is a global program
//     concern — the linker has no concept of block scope. Both the
//     parser and the semantic pass reject extern in non-top-level position.
//
// The parser constructs one of these two types when it sees a declaration
// inside a block and wraps it in DeclStmtAST.
// ─────────────────────────────────────────────────────────────────────────────

using LocalDecl = std::variant<
    ASTPtr<VarDeclAST>,    // let x int = 5
    ASTPtr<FuncDeclAST>    // let f (x int) int = { ... }
>;

// ─────────────────────────────────────────────────────────────────────────────
// DeclStmtAST
//
// A local declaration inside a block body — variable or function only.
//   let x     int    = 5
//   imt limit int    = 100
//   let f     (n int) int = { return n * 2 }
//
// decl holds exactly one of the two LocalDecl alternatives. The parser sets
// isPub = false on whichever node it constructs — the semantic pass may
// assert this as a sanity check but never needs to enforce it by inspecting
// a modifier field on a base pointer.
//
// At top level, declarations live as DeclPtr in ProgramAST::decls.
// This node is only produced when the parser is inside a block body.
// ─────────────────────────────────────────────────────────────────────────────

struct DeclStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DeclStmt;

    LocalDecl decl;

    explicit DeclStmtAST(LocalDecl d)
        : StmtAST(ASTKind::DeclStmt), decl(std::move(d)) {}

    // Convenience helpers — avoid having to write std::get<> at every call site.
    bool isVar()  const { return std::holds_alternative<ASTPtr<VarDeclAST>>(decl);  }
    bool isFunc() const { return std::holds_alternative<ASTPtr<FuncDeclAST>>(decl); }

    VarDeclAST*  asVar()  const { return std::get<ASTPtr<VarDeclAST>>(decl).get();  }
    FuncDeclAST* asFunc() const { return std::get<ASTPtr<FuncDeclAST>>(decl).get(); }

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// BRANCHING STATEMENTS
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// IfStmtAST
//
// The statement form of if — else is optional, no value is produced.
//   if score >= 90 { io.printl("A") }
//   if score >= 90 { io.printl("A") } else { io.printl("F") }
//   if x < 0 { return } else if x == 0 { ... } else { ... }
//
// This is the statement form — contrast with IfExprAST (ExprAST.hpp):
//   IfStmtAST — standalone statement, else optional, no return value
//   IfExprAST — expression context (after '=', in expr position),
//               else required, both branches must produce the same type
//
// elseBranch is one of:
//   nullptr        — no else clause
//   BlockStmtAST   — else { ... }
//   IfStmtAST      — else if ...  (chained — the same node type recursively)
//
// The semantic pass enforces type narrowing inside thenBranch when condition
// is an is-expression (if x is SomeType { ... }).
// ─────────────────────────────────────────────────────────────────────────────

struct IfStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::IfStmt;

    ExprPtr  condition;
    StmtPtr  thenBranch;   // always BlockStmtAST
    StmtPtr  elseBranch;   // nullptr | BlockStmtAST | IfStmtAST

    IfStmtAST() : StmtAST(ASTKind::IfStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// SwitchCaseAST
//
// One case clause inside a switch statement — now a full BaseAST node.
//   case 200, 201, 202: { io.printl("success") }
//   case 1..10:         { io.printl("light") }
//   case 0x41, 0x30..0x39: { handleInput() }
//
// values — one or more match values for this case. Each entry is either:
//   - a plain ExprPtr   (single value: case 200)
//   - a RangeExprAST    (range value:  case 1..10)
// Multiple comma-separated values mean "any of these triggers this case".
//
// body — the block of statements executed when any value matches.
// No fallthrough — each case is fully isolated.
// ─────────────────────────────────────────────────────────────────────────────

struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;

    std::vector<ExprPtr>          values;   // one or more match values / ranges
    ASTPtr<BlockStmtAST> body;     // statements for this case  

    SwitchCaseAST() : BaseAST(ASTKind::SwitchCase) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using SwitchCasePtr = ASTPtr<SwitchCaseAST>;

// ─────────────────────────────────────────────────────────────────────────────
// SwitchStmtAST
//
// Statement-oriented value dispatch — runs statement blocks, produces no value.
// No fallthrough — each case is isolated (unlike C switch).
//
//   switch code {
//       case 200, 201: { io.printl("ok") }
//       case 400:      { io.printl("bad request") }
//       default:       { io.printl("unknown") }
//   }
//
// Compared to match (expression-oriented):
//   switch — statement, no return value, default optional, no pattern matching
//   match  — expression, produces a value, default required, full pattern matching
//
// cases       — non-default case clauses in source order
// defaultBody — nullptr when no default clause was written
// defaultLoc  — location of the 'default' keyword, for error reporting
// ─────────────────────────────────────────────────────────────────────────────

struct SwitchStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchStmt;

    ExprPtr                          subject;
    std::vector<SwitchCasePtr>       cases;
    ASTPtr<BlockStmtAST>    defaultBody;   // nullptr if absent
    std::optional<SourceLocation>    defaultLoc;

    SwitchStmtAST() : StmtAST(ASTKind::SwitchStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// LOOP STATEMENTS
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// ForStmtAST
//
// Iterates over a collection or an inclusive range.
//   for item in items     { io.printl(item) }
//   for i    in 0..10     { io.printl(string(i)) }
//   for v    in mesh.vertices { v.pos = v.pos -> transform }
//
// Both grammar forms (collection and range) map to a single node. The
// semantic pass determines the iteration variable's type from the element
// type of iterable — if iterable is a RangeExprAST, the variable is inferred
// as int; if it is a collection, the variable is inferred as the element type
// of that collection. Explicit type annotation (iterVar->type) overrides inference.
//
// iterVar  — iteration variable parameter (name + optional explicit type)
//            nullptr type = inferred from iterable
// iterable — any expression resolving to a collection or RangeExprAST
// step     — optional step expression for range loops (nullptr if omitted)
// body     — loop body, always a BlockStmtAST
//
// Valid inside body: break, continue, return (exits enclosing function).
// ─────────────────────────────────────────────────────────────────────────────

struct ForStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ForStmt;

    ParamPtr    iterVar;    // iteration variable: name + optional type annotation
    ExprPtr     iterable;   // collection or RangeExprAST (start..end)
    ExprPtr     step;       // optional step (only for range loops, nullptr if omitted)
    StmtPtr     body;       // always a BlockStmtAST

    ForStmtAST() : StmtAST(ASTKind::ForStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// WhileStmtAST
//
// Condition-first loop — condition is tested before each iteration.
//   while n < 5 { n += 1 }
//   while !queue.isEmpty() { process(queue.pop() ?? defaultItem) }
//
// condition — evaluated before each iteration; must resolve to bool
// body      — loop body, always a BlockStmtAST
//
// The loop exits when condition is false or when break is reached.
// ─────────────────────────────────────────────────────────────────────────────

struct WhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::WhileStmt;

    ExprPtr  condition;
    StmtPtr  body;     // BlockStmtAST

    WhileStmtAST() : StmtAST(ASTKind::WhileStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// DoWhileStmtAST
//
// Body-first loop — body executes at least once before condition is checked.
//   do { retries += 1 } while retries < 3
//   do { c = readChar() } while c != '\n'
//
// body      — loop body, always a BlockStmtAST — always runs before first check
// condition — evaluated after each iteration; must resolve to bool
//
// Useful when the exit condition depends on a side effect of the body.
// ─────────────────────────────────────────────────────────────────────────────

struct DoWhileStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DoWhileStmt;

    StmtPtr  body;       // BlockStmtAST — always executes at least once
    ExprPtr  condition;  // checked after body

    DoWhileStmtAST() : StmtAST(ASTKind::DoWhileStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// CONTROL TRANSFER STATEMENTS
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// ReturnStmtAST
//
// Exits the enclosing function, optionally yielding a value.
//   return         — void return (value is nullptr)
//   return 42      — returns an integer literal
//   return a + b   — returns an expression result
//
// The semantic pass checks:
//   - value type matches the enclosing function's declared return type
//   - void return (value = nullptr) only valid in void functions
//   - NOT valid inside parallel for or parallel block bodies
//     (enforced via the isParallel semantic flag on enclosing scopes)
// ─────────────────────────────────────────────────────────────────────────────

struct ReturnStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ReturnStmt;

    std::vector<ExprPtr> values;   // empty for bare 'return'

    ReturnStmtAST() : StmtAST(ASTKind::ReturnStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// BreakStmtAST
//
// Exits the nearest enclosing loop (for, while, do-while).
//   break
//
// Semantic rules enforced by the semantic pass:
//   - only valid directly inside a loop body (for, while, do-while)
//   - NOT valid inside parallel for or parallel block bodies
//   - NOT valid outside of any loop (semantic error)
// ─────────────────────────────────────────────────────────────────────────────

struct BreakStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::BreakStmt;

    BreakStmtAST() : StmtAST(ASTKind::BreakStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ContinueStmtAST
//
// Skips the rest of the current loop iteration and jumps to the next.
//   continue
//
// Semantic rules enforced by the semantic pass:
//   - only valid directly inside a loop body (for, while, do-while)
//   - NOT valid inside parallel for or parallel block bodies
//   - NOT valid outside of any loop (semantic error)
// ─────────────────────────────────────────────────────────────────────────────

struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};
