/// @file ExprAST.hpp
/// 
/// @responsibility Defines all AST nodes that evaluate to a value – literals,
///                 operations, calls, control flow expressions.
/// 
/// @hierarchy BaseAST → ExprAST → [Concrete Expression Nodes]
/// 
/// @related_files
///   - src/parser/ParserExpr.cpp – primary producer of expression nodes
///   - src/ast/StmtAST.hpp – statements that contain expressions
///   - src/semantic/TypeChecker.cpp – consumes for type validation

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "llvm/IR/Intrinsics.h"

#include <string>
#include <optional>

/// @brief Identifies the type of literal value represented by a LiteralExprAST.
/// 
/// The parser maps token types to this enum before constructing the node.
/// The semantic pass uses this to determine the resolved type (Int, Float, etc.).
enum class LiteralKind {
    Int,        // 42
    Float,      // 3.14
    String,     // "hello"
    RawString,  // """raw\nno escaping"""
    Char,       // 'a'
    Hex,        // 0xFF
    Binary,     // 0b1010
    True,       // true
    False,      // false
    Nil,        // nil
    Err,        // err
};

/// @brief Identifies the assignment operator written in source.
/// 
/// Compound operators desugar to `x = x op expr` at semantic time.
enum class AssignOp {
    Assign,       // =
    AddAssign,    // +=
    SubAssign,    // -=
    MulAssign,    // *=
    DivAssign,    // /=
    PowAssign,    // **=
    ModAssign,    // %=
    BitAndAssign, // &=
    BitOrAssign,  // |=
    BitXorAssign, // ^=
    ShlAssign,    // <<=
    ShrAssign,    // >>=
};

/// @brief Identifies the binary operator in a BinaryExprAST.
/// 
/// The parser maps token(s) to this enum before constructing the node.
/// 
/// @note Bitwise operators use single symbols: &, |, ^, <<, >>
///       Logical operators use keywords: and, or
enum class BinaryOp {
    // Arithmetic
    Add,  // +
    Sub,  // -
    Mul,  // *
    Div,  // /
    Pow,  // **
    Mod,  // %

    // Comparison – value equality
    Eq,     // ==
    Ne,     // !=
    Lt,     // <
    Gt,     // >
    Le,     // <=
    Ge,     // >=

    // Logical (short‑circuit) – keywords
    And,  // and
    Or,   // or

    // Bitwise (integer types only) – single symbols
    BitAnd,  // &
    BitOr,   // |
    BitXor,  // ^
    Shl,     // <<
    Shr,     // >>
};

/// @brief Identifies the unary operator in a UnaryExprAST.
enum class UnaryOp {
    Neg,    // -x       arithmetic negation
    Not,    // not x    logical negation
    BitNot, // ~        bitwise NOT
};

/// @brief A scalar literal value – numbers, strings, characters, booleans, nil, err.
/// 
/// @example
///   42         → kind=Int,    value="42"
///   3.14       → kind=Float,  value="3.14"
///   "hello"    → kind=String, value="hello"
///   """raw"""  → kind=RawString, value="raw"
///   'A'        → kind=Char,   value="A"
///   0xFF       → kind=Hex,    value="0xFF"
///   true       → kind=True,   value="true"
///   nil        → kind=Nil,    value="nil"
///   err        → kind=Err,    value="err"
/// 
/// The semantic pass converts the raw lexeme to a typed constant value.
struct LiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::LiteralExpr;

    const LiteralKind kind;
    const InternedString value;   // raw lexeme from the token

    LiteralExprAST(LiteralKind k, InternedString v)
        : ExprAST(ASTKind::LiteralExpr), kind(k), value(std::move(v)) {}
};

/// @brief An array literal – a bracketed list of expressions.
/// 
/// @example
///   [1, 2, 3]
///   ["hello", "world"]
///   []  – empty array literal
/// 
/// The array kind (fixed/slice/dynamic) is inferred from the declared type
/// of the variable being initialised – the literal itself is kind-neutral.
/// The semantic pass sets resolvedType after inference.
struct ArrayLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayLiteralExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<ExprAST*> elements; // may be empty

    // ─── Constructor ─────────────────────────────────────────────────────
    ArrayLiteralExprAST(ArenaSpan<ExprAST*> elems)
        : ExprAST(ASTKind::ArrayLiteralExpr), elements(elems) {}
};

/// @brief One field initializer inside a struct literal expression.
struct FieldInitAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FieldInit;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString name;
    ExprAST* value;

    // ─── Constructor ─────────────────────────────────────────────────────
    FieldInitAST(InternedString n, ExprAST* v)
        : BaseAST(ASTKind::FieldInit), name(n), value(v) {}
};

/// @brief Constructs a value of a named struct type.
/// 
/// @example
///   Vec2 { x = 1.0, y = 2.0 }
///   Point {}  – all fields take their defaults
///   Pair<int, string> { first = 1, second = "one" }
struct StructLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::StructLiteralExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString typeName;
    ArenaSpan<TypeAST*> genericArgs;
    ArenaSpan<FieldInitAST*> inits;

    // ─── Constructor ─────────────────────────────────────────────────────
    StructLiteralExprAST(InternedString n, ArenaSpan<TypeAST*> args, ArenaSpan<FieldInitAST*> in)
        : ExprAST(ASTKind::StructLiteralExpr), typeName(n), genericArgs(args), inits(in) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NAME & ACCESS NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A bare identifier used as an expression.
/// 
/// @example
///   x        – local variable or parameter
///   add      – function name
///   Direction – enum type name (used before .North in Direction.North)
/// 
/// The semantic pass resolves the name against the symbol table and sets
/// resolvedType. If the name resolves to an enum type followed by '.', the
/// parser produces a FieldAccessExprAST – an IdentifierExprAST always refers
/// to a single symbol, never a qualified name.
/// 
/// @field name          The identifier name.
/// @field genericArgs   Generic arguments for generic function instantiation.
struct IdentifierExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IdentifierExpr;

    const InternedString name;
    ArenaSpan<TypeAST*> genericArgs;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    ValueDeclAST* resolvedDecl = nullptr;

    explicit IdentifierExprAST(InternedString n) 
        : ExprAST(ASTKind::IdentifierExpr), name(n) {}
};

/// @brief Accesses a data member (struct field or enum variant) via '.' operator.
/// 
/// @example
///   v.x                     → object = identifier("v"), field = "x"
///   Direction.North         → object = identifier("Direction"), field = "North"
/// 
/// @field object         The object expression.
/// @field field          The field name.
/// @field genericArgs   Generic arguments for generic function access.
struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    ExprAST* object = nullptr;
    const InternedString fieldName;

    FieldAccessExprAST(InternedString n) 
        : ExprAST(ASTKind::FieldAccessExpr), fieldName(n) {}
};

/// @brief Accesses a module member via the ':' operator.
/// 
/// @example
///   math:sqrt(x)         → module = "math", member = "sqrt"
///   std:io.printl("hi")  → nested module access
///   mymod:PI             → reading an exported value
/// 
/// @field module        The module name (left-hand side of `:`).
/// @field member        The member name (right-hand side of `:`).
/// @field genericArgs   Generic arguments for generic function call.
struct ModuleAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ModuleAccessExpr;

    const InternedString moduleName;
    const InternedString memberName;
    ArenaSpan<TypeAST*> genericArgs; // Generic function instantiation

    ModuleAccessExprAST(InternedString mod, InternedString mem) 
        : ExprAST(ASTKind::ModuleAccessExpr),
        moduleName(mod),
        memberName(mem) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CALL & INDEX NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A function call – supports regular calls, generic instantiation,
///        and argument pack (!) for pipeline injection.
/// 
/// @example
///   f(1, 2, 3)                              → callee is IdentifierExprAST
///   Buffer<int>(capacity)                   → genericArgs = [Int]
///   math:sqrt(x)                            → callee is ModuleAccessExprAST
///   x |> map<int, string>(stringFromInt)!   → hasArgPack = true
/// 
/// ─── Generic Instantiation ──────────────────────────────────────────────────
/// Generic arguments are stored in `genericArgs`. The callee remains a plain
/// function reference; the generic arguments are applied at the call site.
/// 
/// ─── Argument Pack (!) ──────────────────────────────────────────────────────
/// `fn(args)!` is not a function call – `!` marks an intentionally incomplete
/// argument list. The upstream value is injected as the **first** argument when
/// `|>` fires. The semantic pass verifies that `hasArgPack` is only true when
/// the call is inside a pipeline step.
/// 
/// ─── Return Type Handling ──────────────────────────────────────────────────
/// For functions returning multiple values, the call site destructures the
/// result into multiple variables: `let value int, ok bool = parseInt("42")`
/// 
/// @field callee        The function being called.
/// @field genericArgs   Generic arguments (empty if none).
/// @field args          Call arguments.
/// @field hasArgPack    True if this is `fn(args)!` (argument pack for pipeline).
struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    ExprAST* callee = nullptr;
    ArenaSpan<TypeAST*> genericArgs;
    ArenaSpan<ExprAST*> args;
    const bool hasArgPack = false;    // true for `fn(args)!`

    CallExprAST(bool a) 
        : ExprAST(ASTKind::CallExpr), hasArgPack(a) {}
};

/// @brief A compiler‑builtin call invoked with the '#' prefix.
/// 
/// @example
///   #sizeof(T)      – compile‑time size of a type in bytes
///   #memcpy(d,s,l)  – memory copy intrinsic
///   #sqrt(x)        – hardware‑accelerated sqrt
/// 
/// The semantic pass validates arguments and sets resolvedType.
/// Codegen maps intrinsicName to the corresponding intrinsic operation.
/// 
/// @field intrinsicName  The intrinsic name ("sizeof", "memcpy", "sqrt", etc.).
/// @field intrinsicID    The LLVM intrinsic ID (set during semantic analysis).
/// @field args           Value arguments in order.
struct IntrinsicCallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IntrinsicCallExpr;

    const InternedString intrinsicName;                 // "sizeof", "memcpy", "sqrt", etc.
    ArenaSpan<ExprAST*> args;                      // value arguments in order
    
    // LLVM intrinsic ID - set during semantic analysis
    // Use std::optional because not all intrinsics map to LLVM intrinsics
    // (e.g., #sizeof, #typeof, #tostr are handled by the compiler directly)
    std::optional<llvm::Intrinsic::ID> intrinsicID = std::nullopt;

    IntrinsicCallExprAST(InternedString n) 
        : ExprAST(ASTKind::IntrinsicCallExpr), intrinsicName(n) {}
};

/// @brief Array element access.
/// 
/// @example
///   nums[2]      → index = 2
/// 
/// The index is runtime-checked. Out-of-bounds access panics unless guarded
/// with `??`: `nums[i] ?? 0`
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Runtime Check**: Indexing a slice (`[_]T`) or dynamic array (`[*]T`)
///    is always runtime-checked. A literal index does not prove in-bounds
///    against a slice of unknown length.
/// 2. **Compile-Time Check**: Indexing a fixed-size array (`[N]T`) with a
///    literal index that is provably less than `N` is checked at compile time.
/// 3. **Panic Handling**: Out-of-bounds access panics unless guarded with `??`.
/// 4. **Type**: The result type is the element type of the array.
/// 
/// @field target        The array being indexed.
/// @field index         The index expression (must be integer type).
struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* target;
    ExprAST* index;

    // ─── Constructor ─────────────────────────────────────────────────────
    IndexExprAST(ExprAST* t, ExprAST* i)
        : ExprAST(ASTKind::IndexExpr), target(t), index(i) {}
};

/// @brief Slice expression – produces a borrowed view over a contiguous range.
/// 
/// @example
///   nums[1..3]   → start = 1, end = 3,   isExclusive = false
///   nums[1..<3]  → start = 1, end = 3,   isExclusive = true  (end excluded)
///   nums[..<2]   → start = nullptr, end = 2, isExclusive = true
///   nums[3..]    → start = 3, end = nullptr, isExclusive = false
///   nums[..]     → start = nullptr, end = nullptr, isExclusive = false
/// 
/// ─── Slice Rules ────────────────────────────────────────────────────────────
/// 1. **Borrowed View**: A slice `[_]T` is a borrowed view – it does not own
///    the underlying memory. The backing array must outlive the slice.
/// 2. **Bounds**: Start defaults to 0, end defaults to the array's length.
/// 3. **Runtime Check**: Slice bounds are runtime-checked. Out-of-bounds
///    access panics unless guarded with `??`.
/// 4. **Inclusive/Exclusive**: `..` is inclusive, `..<` is exclusive.
/// 
/// @field target        The array being sliced.
/// @field start         Inclusive start (nullptr means 0).
/// @field end           End bound (nullptr means array length).
/// @field isExclusive   True for `..<` syntax (end is exclusive).
struct SliceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::SliceExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* target;
    ExprAST* start;      // inclusive, nullptr means 0
    ExprAST* end;        // inclusive or exclusive depending on isExclusive
    const bool isExclusive;

    // ─── Constructor ─────────────────────────────────────────────────────
    SliceExprAST(ExprAST* t, ExprAST* s, ExprAST* e, bool ex = false)
        : ExprAST(ASTKind::SliceExpr), target(t), start(s), end(e), isExclusive(ex) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// OPERATOR NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An infix binary operation.
/// 
/// @example
///   a + b    → op = Add
///   x == y   → op = Eq
///   p and q  → op = And (short‑circuit, logical)
///   a & b    → op = BitAnd (bitwise AND, integer types only)
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Logical Operators**: `and` and `or` are short-circuiting and accept
///    any type (coerced to bool). Result is always bool.
/// 2. **Bitwise Operators**: `&`, `|`, `^`, `<<`, `>>` are integer-only.
/// 3. **Comparison**: `==` and `!=` compare values. Reference equality is
///    not a separate operator (use `&` and compare addresses).
/// 4. **Arithmetic**: `+`, `-`, `*`, `/`, `%`, `**` are numeric-only.
struct BinaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BinaryExpr;

    const BinaryOp op;
    ExprAST* left = nullptr;
    ExprAST* right = nullptr;

    BinaryExprAST(BinaryOp o) 
        : ExprAST(ASTKind::BinaryExpr), op(o) {}
};

/// @brief A prefix unary operation.
/// 
/// @example
///   -x      → op = Neg
///   not x   → op = Not
///   ~x      → op = BitNot
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Ref Operator**: `&x` takes a reference to `x`. `x` must be an lvalue.
///    The result type is `&T` where `T` is the type of `x`.
/// 2. **Bitwise NOT**: `~` is integer-only.
/// 3. **Logical NOT**: `not` accepts any type (coerced to bool). Result is bool.
/// 4. **Arithmetic Negation**: `-` is numeric-only.
struct UnaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnaryExpr;

    const UnaryOp op;
    ExprAST* operand = nullptr;

    UnaryExprAST(UnaryOp o) 
        : ExprAST(ASTKind::UnaryExpr), op(o) {}
};

/// @brief An assignment – plain or compound.
/// 
/// @example
///   x = 5     → op = Assign
///   x += 1    → op = AddAssign (desugars to x = x + 1)
/// 
/// Compound operators desugar to `lhs = lhs op rhs` at semantic time.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Lvalue Required**: The left-hand side must be an assignable lvalue
///    (variable, field access, or index expression).
/// 2. **Const Checking**: Assigning to a `const` variable or `const` field
///    is a semantic error.
/// 3. **Type Matching**: The right-hand side type must match the left-hand
///    side type.
struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    const AssignOp op;
    ExprAST* lhs = nullptr;
    ExprAST* rhs = nullptr;

    AssignExprAST(AssignOp o) 
        : ExprAST(ASTKind::AssignExpr), op(o) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NULLABLE NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The null coalescing operator – provides a fallback value when the LHS is nil or err.
/// 
/// @example
///   value ?? fallback
///   riskyOp() ?? -1
///   lookup() ?? User { id = 0, name = "guest" }
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Sentinel Coverage**: `??` triggers when the left-hand side is `nil`,
///    `err`, or both (for `T?!` types).
/// 2. **Result Type**: The result type is whatever type `rhs` produces,
///    checked against `lhs`'s own type.
/// 
/// @field value          The nullable/fallible value.
/// @field fallback       The fallback expression.
struct NullCoalesceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullCoalesceExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* value;
    ExprAST* fallback;

    // ─── Constructor ─────────────────────────────────────────────────────
    NullCoalesceExprAST(ExprAST* v, ExprAST* f)
        : ExprAST(ASTKind::NullCoalesceExpr), value(v), fallback(f) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// PIPELINE & COMPOSITION NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One step in a pipeline chain – owned by PipelineExprAST.
///
/// Grammar: pipeline_step := expr [ '(' arg_list ')' '!' ] | func_literal
///
/// The `callable` expression is the result of `parseFuncRef()` or an anonymous
/// function expression. If the step includes an argument pack `(args)!`, then
/// `packArgs` is non‑empty and the step is an argument pack step (the `!`
/// annotation).
///
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Argument Pack**: `!` marks an intentionally incomplete argument list.
///    The upstream value is injected as the first argument when `|>` fires.
/// 2. **Curried Functions**: `|>` fills exactly one parameter group. A curried
///    function with remaining unfilled groups is a compile error.
/// 3. **Generic Functions**: Generic functions must be instantiated with
///    explicit type arguments at the pipeline step site.
/// 4. **Nullable/Fallible Steps**: A `~[nullable]` or `~[fallible]` function
///    is forbidden as a pipeline step – the pipeline has no way to narrow it.
///
/// @field callable       The function reference or anonymous function.
/// @field packArgs       Non‑empty for argument pack steps (the `!` annotation).
struct PipelineStepAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineStep;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* callable;
    ArenaSpan<ExprAST*> packArgs;

    // ─── Constructor ─────────────────────────────────────────────────────
    PipelineStepAST(ExprAST* c, ArenaSpan<ExprAST*> p)
        : BaseAST(ASTKind::PipelineStep), callable(c), packArgs(p) {}
};

/// @brief A runtime pipeline chain – seed |> step |> step |> ...
/// 
/// @example
///   42 |> float |> sqrt
///   getUser(id) |> validate |> save
///   v |> Vec2:normalize |> scale(2.0)!
/// 
/// The pipeline short‑circuits on Error when the error library is used.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Left-to-Right**: Pipeline executes left to right at runtime.
/// 2. **Argument Injection**: Each step's upstream value is injected as the
///    first argument when `|>` fires.
/// 3. **Type Chaining**: The output type of each step must match the input
///    type of the next step.
/// 4. **Short-Circuit**: Pipelines short-circuit on Error when using the
///    error library.
/// 
/// @field seed           The initial value.
/// @field steps          Pipeline steps in order (at least one).
struct PipelineExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* seed;
    ArenaSpan<PipelineStepAST*> steps;

    // ─── Constructor ─────────────────────────────────────────────────────
    PipelineExprAST(ExprAST* s, ArenaSpan<PipelineStepAST*> st)
        : ExprAST(ASTKind::PipelineExpr), seed(s), steps(st) {}
};

/// @brief One operand in a +> composition chain – owned by ComposeExprAST.
///
/// Grammar (from LUCID_GRAMMAR.md):
///   compose_operand := expr                          -- function reference
///
/// The callable expression can be:
///   - IdentifierExprAST (plain function name)
///   - FieldAccessExprAST (dotted path)
///   - ModuleAccessExprAST (module:function)
///
/// Generic arguments are applied to the callable (e.g., `toString<int>` becomes
/// callable = IdentifierExprAST("toString") with genericArgs = [int]).
/// 
/// ─── Composition Rules ──────────────────────────────────────────────────────
/// 1. **Single Parameter Group**: Both operands must have exactly one
///    parameter group. Curry functions are forbidden on either side.
/// 2. **Type Matching**: The output type of the left operand must exactly
///    match the input type of the right operand.
/// 3. **Generic Instantiation**: Generic functions must be instantiated
///    with explicit type arguments before composition.
/// 4. **Nullable/Fallible Forbidden**: `~[nullable]` and `~[fallible]`
///    functions are forbidden as composition operands.
/// 5. **Async Composition**: When any operand is `~[async]`, the composed
///    function must be declared `~[async]` and awaited at the call site.
/// 
/// @field callable       The function reference (required).
/// @field genericArgs    Explicit type arguments for generic instantiation.
struct ComposeOperandAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeOperand;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* callable;
    ArenaSpan<TypeAST*> genericArgs;

    // ─── Constructor ─────────────────────────────────────────────────────
    ComposeOperandAST(ExprAST* c, ArenaSpan<TypeAST*> args)
        : BaseAST(ASTKind::ComposeOperand), callable(c), genericArgs(args) {}
};

/// @brief A compile‑time function composition chain – f +> g +> h
/// 
/// Grammar (from LUCID_GRAMMAR.md):
///   compose_expr := expr { '+>' compose_operand }
/// 
/// @example
///   const process = validate +> transform +> render
///   const intToString = identity<int> +> toString<int> +> trim
/// 
/// ─── Key Characteristics ──────────────────────────────────────────────────
/// - Compile-time: Produces a new function without executing anything.
/// - Type Matching: Strict – output type of left must exactly match input type of right.
/// - No Qualifiers: `~[async]` or `~[nullable]` operands are forbidden.
/// - Generic Instantiation: Explicit type arguments required for generic functions.
/// 
/// @field left           The leftmost operand.
/// @field operands       Right‑hand operands in order (at least one).
struct ComposeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* left;
    ArenaSpan<ComposeOperandAST*> operands;

    // ─── Constructor ─────────────────────────────────────────────────────
    ComposeExprAST(ExprAST* l, ArenaSpan<ComposeOperandAST*> ops)
        : ExprAST(ASTKind::ComposeExpr), left(l), operands(ops) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FUNCTION NODES
// ─────────────────────────────────────────────────────────────────────────────

// ─── AnonFuncExprAST ─────────────────────────────────────────────────────

/// @brief An anonymous function expression – a function value without a name.
/// 
/// @example
///   (x int) -> int { return x * 2 }
///   (a int)(b int) -> int { return a + b }   – adjacent groups in bound_cluster
/// 
/// Like FuncDeclAST, the bound_cluster groups are tracked separately from the
/// function type.
struct AnonFuncExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AnonFuncExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    FuncTypeAST* funcType;
    StmtAST* body;

    // ─── Semantic Fields (set by Sema) ────────────────────────────────
    ArenaSpan<CapturedVariable> captures;
    bool hasClosure = false;
    bool isReturned = false;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::Function* closureFunction = nullptr;
    llvm::StructType* environmentType = nullptr;

    bool hasParams() const { return funcType && !funcType->params.empty(); }

    // ─── Constructor ─────────────────────────────────────────────────────
    AnonFuncExprAST(FuncTypeAST* ft, StmtAST* b)
        : ExprAST(ASTKind::AnonFuncExpr), funcType(ft), body(b) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONTROL FLOW EXPRESSION NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The expression form of if – both branches required, both produce the same type.
/// 
/// @example
///   if score >= 60 ?? "pass" else "fail"
///   if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
/// 
/// Grammar: if_expr := 'if' expr '??' expr 'else' expr
/// 
/// This is distinct from IfStmtAST (in StmtAST.hpp) where else is optional
/// and no value is produced.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Else Required**: Both branches must be present.
/// 2. **Type Matching**: Both branches must produce compatible types.
/// 3. **Chaining**: Chained if-expressions are right-associative.
/// 
/// @field condition       The condition expression.
/// @field thenBranch      The then branch (expression).
/// @field elseBranch      The else branch (expression).
struct IfExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IfExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* condition;
    ExprAST* thenBranch;
    ExprAST* elseBranch;

    // ─── Constructor ─────────────────────────────────────────────────────
    IfExprAST(ExprAST* cond, ExprAST* then_, ExprAST* else_)
        : ExprAST(ASTKind::IfExpr), condition(cond), thenBranch(then_), elseBranch(else_) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// OTHER EXPRESSION NODES
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An inclusive range literal – lo..hi or lo..<hi.
/// 
/// @example
///   0..10   – used in for loops: for i int in 0..10
///   1..10   – used in match range patterns: case 1..10
///   1..3    – used in slice index: nums[1..3]
/// 
/// Both ends are inclusive for '..' – 0..10 iterates 0,1,2,...,10 (11 steps).
/// When isExclusive is true, the end is exclusive (..< syntax).
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// 1. **Range Positions**: A range only appears in three positions:
///    - Range iteration in `for`
///    - Slice bounds
///    - `switch` case values
/// 2. **No Standalone Type**: A range is not a standalone collection value
///    with its own type – there is no general-purpose range type.
/// 3. **Literal Bounds**: In `switch` cases, both bounds must be literals.
/// 
/// @field lo             Start (inclusive).
/// @field hi             End (inclusive/exclusive depends on flag).
/// @field isExclusive    True for `..<` syntax (end is exclusive).
/// @note start and end must always be a positive integer
struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprAST* lo = nullptr;   // start (inclusive)
    ExprAST* hi = nullptr;   // end (inclusive/exclusive depends on flag)
    const bool isExclusive = false;   // true for ..<

    RangeExprAST(bool ex) 
        : ExprAST(ASTKind::RangeExpr), isExclusive(ex) {}
};
