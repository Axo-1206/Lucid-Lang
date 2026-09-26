/**
 * @file ExprAST.hpp
 *
 * @responsibility The AST nodes for expressions. Every expression form
 *                 in the grammar is one node here; the operators are
 *                 represented by `BinaryExprAST` and `UnaryExprAST`,
 *                 not by dedicated nodes per operator.
 *
 * @hierarchy BaseAST → ExprAST → [Concrete Expression Nodes]
 *
 * ─── Design: twelve expression forms ──────────────────────────────────────
 * An expression is exactly one of:
 *
 *   - a literal               (`42`, `"hi"`, `'a'`, `true`, `nil`)
 *   - an identifier           (`x`, `Person`, `createPerson`)
 *   - an array literal        (`[1, 2, 3]`)
 *   - a field access          (`a.b`)
 *   - an index                (`a[i]`)
 *   - a call                  (`f(args)`)
 *   - a lambda                (`(p) -> expr`)
 *   - a start expression      (`start f(args)`)
 *   - a unary operation       (`-x`, `not x`, `~x`)
 *   - a binary operation      (`a + b`, `a == b`, `a ?? b`, ...)
 *   - an assignment           (`a = b`, `a += b`, ...)
 *   - a parenthesized expr    (`(expr)`)
 *
 * `a.b` is a field access, `a[i]` is an index, `f(args)` is a call.
 * Module member access (`mod.Member`) is a field access whose object
 * resolves to a module; Sema sets the flag.
 *
 * ─── Design: one node per operator category, not per operator ─────────────
 * `BinaryExprAST` covers all binary operators; `UnaryExprAST` covers all
 * unary operators. The operator is a field on the node, not a
 * discriminator between node types. This is the standard Pratt-parser
 * AST shape: the parser knows the operator, and a single node type with
 * an op field is simpler to dispatch on than a dozen node types.
 *
 * ─── Design: no pipeline, no slice, no if-expression ──────────────────────
 * The grammar has no `|>`, no `[_]T` slice, no `if cond ?? a else b`
 * ternary. Those were in the previous design and were removed. The
 * expression grammar is smaller as a result.
 *
 * ─── Design: `??` is a binary operator ────────────────────────────────────
 * The null-coalescing operator is `BinaryOp::NullCoalesce`. It is
 * syntactically a binary operator with a `nil` check on the left; there
 * is no separate node type. This is a change from the previous design,
 * which had a dedicated `NullCoalesceExprAST`.
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// LiteralKind
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The kind of literal a `LiteralExprAST` holds.
///
/// The lexer distinguishes these by token type; the parser maps each
/// token type to a `LiteralKind`. Sema converts the raw lexeme to a
/// typed constant value.
enum class LiteralKind : uint8_t {
    Int,     // 42
    Float,   // 3.14
    String,  // "hello"
    RawString, // """raw text"""
    Char,    // 'a'
    Hex,     // 0xFF
    Binary,  // 0b1010
    Octal,   // 0o777
    True,    // true
    False,   // false
    Nil,     // nil
    Unknown, // broken or ambiguous
};

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOp and UnaryOp
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The binary operators.
///
/// The set is fixed; the grammar lists these operators in §6.10. There
/// is no user-defined operator overloading. `NullCoalesce` is `??`.
///
/// The categories (arithmetic, comparison, logical, bitwise) are
/// distinguished by the operand types Sema resolves, not by which
/// operator is used. `+` on `int` is arithmetic addition; `+` on
/// `string` is concatenation; both use `BinaryOp::Add`.
enum class BinaryOp : uint8_t {
    // Arithmetic
    Add,    // +
    Sub,    // -
    Mul,    // *
    Div,    // /
    Mod,    // %
    Pow,    // **

    // Comparison
    Eq,     // ==
    Ne,     // !=
    Lt,     // <
    Le,     // <=
    Gt,     // >
    Ge,     // >=

    // Logical (short-circuit, keywords)
    And,    // and
    Or,     // or

    // Bitwise
    BitAnd, // &
    BitOr,  // |
    BitXor, // ^
    Shl,    // <<
    Shr,    // >>

    // Null coalescing
    NullCoalesce, // ??
};

/// @brief The unary operators.
enum class UnaryOp : uint8_t {
    Neg,    // -
    Not,    // not
    BitNot, // ~
};

// ─────────────────────────────────────────────────────────────────────────────
// AssignOp
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The assignment operators.
///
/// A compound assignment (`+=`, `-=`, etc.) is stored with its own
/// operator tag; Sema desugars `x op= y` to `x = x op y`. There is no
/// `AssignOp` for `**=` — the grammar's assignment operator list does
/// not include it (see §6.11).
enum class AssignOp : uint8_t {
    Assign,       // =
    AddAssign,    // +=
    SubAssign,    // -=
    MulAssign,    // *=
    DivAssign,    // /=
    ModAssign,    // %=
    BitAndAssign, // &=
    BitOrAssign,  // |=
    BitXorAssign, // ^=
    ShlAssign,    // <<=
    ShrAssign,    // >>=
};

// ─────────────────────────────────────────────────────────────────────────────
// LiteralExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A scalar literal value.
///
/// The parser stores the raw lexeme from the source; Sema converts it to
/// a typed constant. The `kind` field tells Sema how to interpret the
/// lexeme (a hex literal, a decimal integer, a string, ...).
///
/// @example
///   42         → kind=Int,    value="42"
///   3.14       → kind=Float,  value="3.14"
///   "hello"    → kind=String, value="hello"
///   """raw"""  → kind=RawString, value="raw"
///   'A'        → kind=Char,   value="A"
///   0xFF       → kind=Hex,    value="0xFF"
///   0b1010     → kind=Binary, value="0b1010"
///   true       → kind=True,   value="true"
///   nil        → kind=Nil,    value="nil"
struct LiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::LiteralExpr;

    const LiteralKind   kind;
    const InternedString value;   // raw lexeme from the token

    LiteralExprAST(LiteralKind k, InternedString v)
        : ExprAST(ASTKind::LiteralExpr), kind(k), value(v) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// IdentifierExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A bare identifier used as an expression.
///
/// The parser produces this node for any identifier that is not part of
/// a field access, index, or call. Sema resolves the name against the
/// current scope chain: local bindings, parameters, module-level
/// bindings, then the module's imports' exports.
///
/// An identifier in expression position may resolve to:
///   - a variable binding,
///   - a function (a top-level `FN`, producing a function value),
///   - a table name (when it is the object of a `.`, `.ADD(...)`, or
///     similar table operation),
///   - a parameter.
///
/// Sema records what the identifier resolved to in `resolvedDecl`. The
/// AST node does not distinguish the cases; the resolution does.
///
/// @example
///   x               → a local variable
///   createPerson    → a function
///   Person          → a table name (used before `.` or `[...]`)
struct IdentifierExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IdentifierExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString name;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// The declaration this identifier resolved to. May be a `VarDeclAST`,
    /// `FnDeclAST`, `ParamAST`, or `TableDeclAST`.
    DeclAST* resolvedDecl = nullptr;

    explicit IdentifierExprAST(InternedString n)
        : ExprAST(ASTKind::IdentifierExpr), name(n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ArrayLiteralExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An array literal: `[1, 2, 3]` or `[]`.
///
/// The array's element type is inferred from the elements; the array's
/// *kind* (dynamic vs. fixed) is determined by the declared type of the
/// binding the literal initializes. The literal itself is kind-neutral.
///
/// The parser does not decide the array kind. It produces a
/// `ArrayLiteralExprAST` with the elements in order; Sema resolves the
/// literal's type from context.
///
/// An empty literal (`[]`) requires a declared type to infer the element
/// type from. Sema rejects an empty literal that has no target type.
///
/// @example
///   [1, 2, 3]
///   ["hello", "world"]
///   []                          -- requires a target type
///   [[1, 2], [3, 4]]            -- nested
struct ArrayLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayLiteralExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<ExprAST*> elements;   // may be empty

    explicit ArrayLiteralExprAST(ArenaSpan<ExprAST*> elems)
        : ExprAST(ASTKind::ArrayLiteralExpr), elements(elems) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldAccessExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A field access: `object.field`.
///
/// The node covers three distinct operations, distinguished by Sema
/// after resolving the object's type:
///
///   - **Cell access.** `row.name` reads the `name` cell of the row
///     `row` refers to. `row.name = ...` writes the cell.
///   - **Column view.** `Person.name` (where `Person` is a table)
///     produces a view over the `name` column, usable in a `for` loop
///     or an aggregation call.
///   - **Module member access.** `math.sqrt` (where `math` is a
///     module) accesses the module's export. Sema sets
///     `isModuleAccess` when the object resolves to a module rather
///     than a value or a table.
///
/// The parser produces one `FieldAccessExprAST` per `a.b` regardless of
/// which case it is. Sema classifies the node and sets the flags it
/// needs. The AST has no syntactic distinction between the three.
///
/// @example
///   row.name        → object = IdentifierExpr("row"), field = "name"
///   Person.name     → object = IdentifierExpr("Person"), field = "name"
///   Person.ADD      → object = IdentifierExpr("Person"), field = "ADD"
///   math.sqrt       → object = IdentifierExpr("math"), field = "sqrt"
///                     (Sema sets isModuleAccess)
struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST*            object;
    const InternedString fieldName;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// True if the object resolved to a module name. Sema sets this;
    /// when true, `fieldName` names an exported member of that module.
    bool isModuleAccess = false;

    /// True if the object resolved to a table's name and the field
    /// resolved to a built-in table method (`ADD`, `FIND`, `COUNT`, ...).
    /// Sema sets this when the object is a table name and the field is
    /// a recognized method.
    bool isTableMethod = false;

    /// True if the object resolved to a table's name and the field
    /// resolved to a column. Sema sets this when the access produces a
    /// column view.
    bool isColumnView = false;

    /// The resolved column, when the access is a column view or a cell
    /// access. Null for a module member access.
    ColumnDeclAST* resolvedColumn = nullptr;

    FieldAccessExprAST(InternedString f)
        : ExprAST(ASTKind::FieldAccessExpr), fieldName(f) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// IndexExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An index: `container[index]`.
///
/// Used for two kinds of access:
///
///   - **Table row.** `Person[i]` accesses the row at index `i` of the
///     `Person` sheet. The result is `&Person`. Out-of-bounds panics
///     (grammar §7.3); `T.at(i)` returns `nil` instead.
///   - **Array element.** `arr[i]` accesses the element at index `i`.
///     The result is the array's element type. Out-of-bounds panics;
///     the fallback is `??`.
///
/// The parser produces the same node for both. Sema resolves which by
/// the container's type.
///
/// @field target  The container being indexed (a table name or an array
///                expression).
/// @field index   The index expression.
struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    ExprAST* target = nullptr;
    ExprAST* index  = nullptr;

    IndexExprAST(ExprAST* t, ExprAST* i)
        : ExprAST(ASTKind::IndexExpr), target(t), index(i) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CallExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A function call: `callee(args)`.
///
/// The callee is a function value — a named `FN`, a lambda, a table
/// method (`Person.ADD`, `Person.FIND`), or a module function
/// (`math.sqrt`). Sema resolves the callee and checks the arguments
/// against the signature.
///
/// There is no currying. A call produces exactly one result, or none
/// (a `unit`-returning function). A function that needs to produce
/// multiple values returns a row reference (`&T`), and the caller
/// accesses cells.
///
/// @example
///   createPerson("alice", 30)
///   math.sqrt(2.0)
///   Person.ADD("alice", 30)
///   f(x)(y)                     -- not allowed: no currying
struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    ExprAST*            callee;
    ArenaSpan<ExprAST*> args;

    CallExprAST()
        : ExprAST(ASTKind::CallExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// LambdaExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A lambda: `(p1, p2) -> expr`.
///
/// A single-expression function value. The body is one expression, not
/// a block. Lambdas are legal wherever a function type is expected: a
/// `FIND` predicate, a parameter typed `(...) -> R`, a function-typed
/// column in a fixed table.
///
/// A lambda has no captures: its body sees only its own parameters and
/// the module's top-level declarations. This is what makes a lambda a
/// compile-time-known code address and keeps "every function value is a
/// bare code pointer" true (grammar §6.9).
///
/// The compiler lowers each lambda to a top-level function. The lambda
/// node is the source representation; the lowered function is invisible
/// at the AST level.
///
/// @example
///   (p) -> p.age < 18
///   (a, b) -> a + b
///   () -> 42
struct LambdaExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::LambdaExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<ParamAST*> params;
    ExprAST*             body = nullptr;

    LambdaExprAST() : ExprAST(ASTKind::LambdaExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// StartExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A start expression: `start call_expr`.
///
/// Launches a `@sequence` function and produces a `&Coroutine` handle
/// for the running instance. The call must invoke a function tagged
/// `@sequence`; calling an ordinary `FN` with `start` is a compile
/// error, and calling a `@sequence` function without `start` is also a
/// compile error (grammar §9.2.4).
///
/// The handle is a host-backed opaque type. To interact with it, use the
/// standard library's `stop`, `isDone`, etc.
///
/// @example
///   let c: &Coroutine = start playIntro()
///   start playIntro()          -- fire-and-forget; the result is discarded
struct StartExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::StartExpr;

    /// The call expression being started. Its callee must be a
    /// `@sequence` function.
    CallExprAST* call = nullptr;

    explicit StartExprAST(CallExprAST* c)
        : ExprAST(ASTKind::StartExpr), call(c) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// UnaryExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A prefix unary operation: `-x`, `not x`, `~x`.
///
/// The operator is a field. Sema checks the operand's type against the
/// operator: `-` is numeric, `~` is integer, `not` accepts any type that
/// the truthiness table treats as a boolean.
///
/// @example
///   -x       → op = Neg
///   not x    → op = Not
///   ~x       → op = BitNot
struct UnaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnaryExpr;

    const UnaryOp op;
    ExprAST*      operand = nullptr;

    explicit UnaryExprAST(UnaryOp o)
        : ExprAST(ASTKind::UnaryExpr), op(o) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// BinaryExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An infix binary operation.
///
/// The operator is a field; one node covers arithmetic, comparison,
/// logical, bitwise, and null-coalescing operators. `and` and `or` are
/// short-circuit; the compiler emits the short-circuit sequence
/// regardless of the operand types.
///
/// @example
///   a + b        → op = Add
///   x == y       → op = Eq
///   p and q      → op = And
///   a & b        → op = BitAnd
///   x ?? y       → op = NullCoalesce
struct BinaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BinaryExpr;

    const BinaryOp op;
    ExprAST*       left  = nullptr;
    ExprAST*       right = nullptr;

    explicit BinaryExprAST(BinaryOp o)
        : ExprAST(ASTKind::BinaryExpr), op(o) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// AssignExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An assignment: `lhs = rhs` or a compound form.
///
/// The left-hand side must be an lvalue: an identifier, a field access
/// on an lvalue, or an index on an lvalue. Sema checks this.
///
/// Compound assignments desugar to `lhs = lhs op rhs` at Sema time; the
/// AST carries the compound operator tag, and Sema produces the
/// equivalent tree. There is no separate assignment *statement* node;
/// an assignment is an expression, wrapped in an `ExprStmtAST` when it
/// appears as a statement.
///
/// @example
///   x = 5        → op = Assign
///   x += 1       → op = AddAssign
struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    const AssignOp op;
    ExprAST*       lhs = nullptr;
    ExprAST*       rhs = nullptr;

    explicit AssignExprAST(AssignOp o)
        : ExprAST(ASTKind::AssignExpr), op(o) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ParenExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A parenthesized expression: `(expr)`.
///
/// Parentheses do not change what the expression means once the tree is
/// built — the tree's shape already encodes precedence. The node exists
/// so that tooling (the LSP, a future transpiler, the JSON dumper) can
/// reconstruct the source with the parentheses the user wrote.
///
/// The bytecode compiler and Sema descend through `inner` transparently.
///
/// @example
///   (1 + 2) * 3     → the paren node wraps `1 + 2`; the outer `*` sees
///                     the paren as its left operand.
struct ParenExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ParenExpr;

    ExprAST* inner = nullptr;

    explicit ParenExprAST(ExprAST* e)
        : ExprAST(ASTKind::ParenExpr), inner(e) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// RangeExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A range expression: `lo..hi` or `lo..<hi`.
///
/// A range is not a first-class value. It appears in exactly two
/// positions:
///
///   - as the iterable of a `for` loop:
///       for i: int in 0..<10 { ... }
///       for i: int in 0..10..2 { ... }      -- with an optional step
///
///   - as a `case_value` in a `switch` case:
///       case 90..100: { ... }
///
/// It cannot be assigned to a variable, passed to a function, or used
/// in arithmetic. Sema reports a diagnostic if a range appears outside
/// the two valid positions.
///
/// The `step` field is only used in a `for` iterable; a range in a
/// `switch` case has `step == nullptr`. A step of zero is a compile
/// error; a negative step counts down.
///
/// @field lo           The inclusive lower bound.
/// @field hi           The upper bound (inclusive for `..`, exclusive
///                     for `..<`).
/// @field step         The step, if written with a second range operator.
///                     Null when no step was written.
/// @field isExclusive  True for `..<` (upper bound excluded), false for
///                     `..` (inclusive).
struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprAST* lo = nullptr;
    ExprAST* hi = nullptr;
    ExprAST* step = nullptr;
    const bool isExclusive = false;

    explicit RangeExprAST(bool exclusive)
        : ExprAST(ASTKind::RangeExpr), isExclusive(exclusive) {}
};