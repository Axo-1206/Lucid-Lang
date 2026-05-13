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
//   x |> validate |> save  — pipeline as a statement
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
// DeclStmtAST
//
// A local declaration inside a block body – now supports ANY declaration kind.
// Previously restricted to only VarDeclAST and FuncDeclAST. After refactoring,
// DeclStmtAST can hold any DeclAST node (type alias, struct, enum, trait, impl,
// from, var, func). The parser enforces which declaration kinds are semantically
// valid inside a block (visibility modifiers and attributes are rejected locally).
//
// Usage in the AST:
//   - BlockStmtAST::stmts contains StmtPtr, which may point to a DeclStmtAST.
//   - The decl field points to the actual declaration node (e.g., TypeAliasDeclAST).
//
// Semantic handling:
//   - The semantic pass visits DeclStmtAST, then dispatches to the specific
//     declaration via decl->accept(visitor). The visitor then registers the
//     declared name in the current block's type/value scope depending on the
//     declaration kind.
//   - Types declared locally (type aliases, structs, enums) are visible only
//     inside the block where they appear – scoping follows block nesting.
//
// Example AST structure for a local type alias:
//   BlockStmtAST
//     └─ DeclStmtAST
//          └─ decl: TypeAliasDeclAST { name = "Vec2", aliasedType = ... }
// ─────────────────────────────────────────────────────────────────────────────
struct DeclStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::DeclStmt;

    DeclPtr decl;   // now holds any declaration (type alias, struct, var, func, ...)

    explicit DeclStmtAST(DeclPtr d) : StmtAST(ASTKind::DeclStmt), decl(std::move(d)) {}

    // Convenience helpers – use decl->isa<T>() directly in most cases.
    bool isVar()        const { return decl && decl->isa<VarDeclAST>(); }
    bool isFunc()       const { return decl && decl->isa<FuncDeclAST>(); }
    bool isTypeAlias()  const { return decl && decl->isa<TypeAliasDeclAST>(); }
    bool isStruct()     const { return decl && decl->isa<StructDeclAST>(); }
    bool isImplDecl()   const { return decl && decl->isa<ImplDeclAST>(); }
    bool isFromDecl()   const { return decl && decl->isa<FromDeclAST>(); }
    bool isEnum()       const { return decl && decl->isa<EnumDeclAST>(); }
    bool isUseDecl()    const { return decl && decl->isa<UseDeclAST>(); }
    bool isTrait()      const { return decl && decl->isa<TraitDeclAST>(); }

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
//   - NOT valid outside of any loop (semantic error)
// ─────────────────────────────────────────────────────────────────────────────
struct ContinueStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::ContinueStmt;

    ContinueStmtAST() : StmtAST(ASTKind::ContinueStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ═════════════════════════════════════════════════════════════════════════════
// MULTI‑ASSIGNMENT STATEMENT
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// MultiVarDeclAST – multiple variable declaration (with let/const)
//
// Grammar:
//   multi_assign := decl_keyword var_spec { ',' var_spec } '=' expr
//   var_spec     := IDENTIFIER type_ann
//   decl_keyword := 'let' | 'const'
//
// Introduces new variables into the current scope. All variables share the
// same keyword. Each variable has an explicit type. The RHS must be a single
// expression that returns as many values as there are variables.
//
// Examples:
//   let q int, r int = divmod(10, 3)
//   const w int, h int = getScreenSize()
//
// keyword – let or const
// vars    – list of (name, type) pairs
// rhs     – initialiser expression
// ─────────────────────────────────────────────────────────────────────────────
struct MultiVarDeclAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::MultiVarDecl;

    DeclKeyword keyword;
    std::vector<std::pair<InternedString, TypePtr>> vars;
    ExprPtr rhs;

    MultiVarDeclAST() : StmtAST(ASTKind::MultiVarDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// MultiAssignStmtAST – multi‑assignment to existing variables (no let/const)
//
// Grammar:
//   multi_assign_stmt := expr_lhs { ',' expr_lhs } '=' expr
//   expr_lhs          := IDENTIFIER | expr '.' IDENTIFIER | expr '[' expr ']'
//
// Reassigns multiple lvalues from a single RHS expression that returns
// as many values as there are lhs expressions.
//
// Examples:
//   a, b = f()
//   x.y, arr[i] = g()
//   p.x, q.y = h()
//
// lhs – vector of expressions, each must be an assignable lvalue.
// rhs – the single expression producing the values to assign.
// ─────────────────────────────────────────────────────────────────────────────
struct MultiAssignStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::MultiAssignStmt;

    std::vector<ExprPtr> lhs;   // left‑hand side lvalues
    ExprPtr rhs;                // right‑hand side expression (single)

    MultiAssignStmtAST() : StmtAST(ASTKind::MultiAssignStmt) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};