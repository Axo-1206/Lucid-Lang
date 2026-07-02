/**
 * @file ExprAST.hpp
 *
 * @responsibility Defines all AST nodes that evaluate to a value – literals,
 *                 operations, calls, control flow expressions.
 *
 * @hierarchy BaseAST → ExprAST → [Concrete Expression Nodes]
 *
 * @related_files
 *   - src/parser/ParserExpr.cpp – primary producer of expression nodes
 *   - src/ast/StmtAST.hpp – statements that contain expressions
 *   - src/semantic/TypeChecker.cpp – consumes for type validation
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"

#include <string>
#include <optional>

/**
 * @brief Identifies the type of literal value represented by a LiteralExprAST.
 *
 * The parser maps token types to this enum before constructing the node.
 * The semantic pass uses this to determine the resolved type (Int, Float, etc.).
 */
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

/**
 * @brief Identifies the assignment operator written in source.
 *
 * Compound operators desugar to `x = x op expr` at semantic time.
 */
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

/**
 * @brief Identifies the binary operator in a BinaryExprAST.
 *
 * The parser maps token(s) to this enum before constructing the node.
 *
 * @note Bitwise operators use single symbols: &, |, ^, <<, >>
 *       Logical operators use keywords: and, or
 */
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

/**
 * @brief Identifies the unary operator in a UnaryExprAST.
 */
enum class UnaryOp {
    Neg,    // -x       arithmetic negation
    Not,    // not x    logical negation
    BitNot, // ~        bitwise NOT
};

/**
 * @brief A scalar literal value – numbers, strings, characters, booleans, nil, err.
 *
 * @example
 *   42         → kind=Int,    value="42"
 *   3.14       → kind=Float,  value="3.14"
 *   "hello"    → kind=String, value="hello"
 *   """raw"""  → kind=RawString, value="raw"
 *   'A'        → kind=Char,   value="A"
 *   0xFF       → kind=Hex,    value="0xFF"
 *   true       → kind=True,   value="true"
 *   nil        → kind=Nil,    value="nil"
 *   err        → kind=Err,    value="err"
 *
 * The semantic pass converts the raw lexeme to a typed constant value.
 */
struct LiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::LiteralExpr;

    LiteralKind kind;
    InternedString value;   // raw lexeme from the token

    LiteralExprAST(LiteralKind k, InternedString v)
        : ExprAST(ASTKind::LiteralExpr), kind(k), value(std::move(v)) {}
};

/**
 * @brief An array literal – a bracketed list of expressions.
 *
 * @example
 *   [1, 2, 3]
 *   ["hello", "world"]
 *   []  – empty array literal
 *
 * The array kind (fixed/slice/dynamic) is inferred from the declared type
 * of the variable being initialised – the literal itself is kind-neutral.
 * The semantic pass sets resolvedType after inference.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Empty Array**: `[]` is an empty array literal. Its type must be
 *    inferred from context (assignment type annotation).
 * 2. **Element Types**: All elements must have the same type. If types differ,
 *    the semantic pass emits a compile error.
 * 3. **Type Inference**: The array's element type is inferred from the
 *    declared type of the variable being initialized.
 */
struct ArrayLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayLiteralExpr;

    ArenaSpan<ExprPtr> elements; // may be empty

    ArrayLiteralExprAST() : ExprAST(ASTKind::ArrayLiteralExpr) {}
};

/**
 * @brief One field initializer inside a struct literal expression.
 *
 * @example
 *   x = 1.0
 *   name = "hello"
 *   position = Vec2 { x = 0, y = 0 }
 *
 * Now a proper BaseAST node so the semantic pass and tools can walk the
 * field‑to‑expression binding uniformly.
 */
struct FieldInitAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FieldInit;

    InternedString name; // field name being initialised
    ExprPtr value;      // initialiser expression

    FieldInitAST() : BaseAST(ASTKind::FieldInit) {}
    FieldInitAST(InternedString n, ExprPtr v)
        : BaseAST(ASTKind::FieldInit), name(n), value(v) {}
};
using FieldInitPtr = FieldInitAST*;

/**
 * @brief Constructs a value of a named struct type.
 *
 * @example
 *   Vec2 { x = 1.0, y = 2.0 }
 *   Point {}  – all fields take their defaults
 *   Pair<int, string> { first = 1, second = "one" }
 *
 * ─── Const Field Rules ──────────────────────────────────────────────────────
 * 1. **Const Fields with Default**: If a const field has a default value,
 *    it can be omitted during initialization.
 * 2. **Const Fields without Default**: If a const field does NOT have a
 *    default value, it must be provided during initialization.
 * 3. **Override Allowed**: A const field with a default value may be
 *    overridden during initialization (providing a different value is allowed).
 * 4. **Mutable Fields**: Follow the same rules – if they have defaults,
 *    they can be omitted; if not, they must be provided.
 *
 * @field typeName      The name of the struct type.
 * @field genericArgs   Generic arguments (empty if non-generic).
 * @field inits         Field initializers (field = expr entries).
 * @field instantiatedType  Resolved type (set during semantic analysis).
 */
struct StructLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::StructLiteralExpr;

    InternedString typeName;               // "Vec2", "Color", "Pair"
    ArenaSpan<TypePtr> genericArgs;        // empty if non‑generic
    ArenaSpan<FieldInitPtr> inits;          // field = expr entries
    NamedTypeAST* instantiatedType;         // semantic cache (raw pointer)

    StructLiteralExprAST() : ExprAST(ASTKind::StructLiteralExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NAME & ACCESS NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A bare identifier used as an expression.
 *
 * @example
 *   x        – local variable or parameter
 *   add      – function name
 *   Direction – enum type name (used before .North in Direction.North)
 *
 * The semantic pass resolves the name against the symbol table and sets
 * resolvedType. If the name resolves to an enum type followed by '.', the
 * parser produces a FieldAccessExprAST – an IdentifierExprAST always refers
 * to a single symbol, never a qualified name.
 *
 * @field name          The identifier name.
 * @field genericArgs   Generic arguments for generic function instantiation.
 */
struct IdentifierExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IdentifierExpr;

    InternedString name;
    ArenaSpan<TypePtr> genericArgs;

    explicit IdentifierExprAST() : ExprAST(ASTKind::IdentifierExpr) {}
};

/**
 * @brief Accesses a data member (struct field or enum variant) via '.' operator.
 *
 * @example
 *   v.x                     → object = identifier("v"), field = "x"
 *   Direction.North         → object = identifier("Direction"), field = "North"
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Field Access**: `object.field` accesses a struct field. If the object
 *    is `~[const]`, the field access is read-only.
 * 2. **Enum Variant**: `EnumName.Variant` accesses an enum variant.
 * 3. **Generic Functions**: When the field refers to a generic function,
 *    the FieldAccessExprAST becomes the entity of a generic instantiation.
 *
 * @field object         The object expression.
 * @field field          The field name.
 * @field genericArgs   Generic arguments for generic function access.
 */
struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    ExprPtr object;
    InternedString fieldName;
    ArenaSpan<TypePtr> genericArgs; // Generic function instantiation

    FieldAccessExprAST() : ExprAST(ASTKind::FieldAccessExpr) {}
};

/**
 * @brief Accesses a module member via the ':' operator.
 *
 * @example
 *   math:sqrt(x)         → module = "math", member = "sqrt"
 *   std:io:printl("hi")  → nested module access
 *   mymod:PI             → reading an exported value
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - `:` is for module access, never struct field access
 * - Module members are always read-only from outside the module
 * - `:` never produces an l-value
 * - The left-hand side must be a module name, not a struct value
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Read-Only**: Module members obtained via `:` are always read-only,
 *    regardless of the member's internal mutability.
 * 2. **Depth Guarantee**: "Always read-only" applies to everything reachable
 *    through `:` – a struct obtained via `:` is treated as `const` at every
 *    field depth.
 * 3. **No Assignment**: `module:member = ...` is always a compile error.
 * 4. **Function Calls**: `module:func(args)` calls an exported function.
 * 5. **Generic Functions**: `module:genericFunc<T>(args)` calls a generic
 *    exported function.
 *
 * @field module        The module name (left-hand side of `:`).
 * @field member        The member name (right-hand side of `:`).
 * @field genericArgs   Generic arguments for generic function call.
 */
struct ModuleAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ModuleAccessExpr;

    InternedString moduleName;
    InternedString memberName;
    ArenaSpan<TypePtr> genericArgs; // Generic function instantiation

    ModuleAccessExprAST() : ExprAST(ASTKind::ModuleAccessExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CALL & INDEX NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A function call – supports regular calls, generic instantiation,
 *        and argument pack (!) for pipeline injection.
 *
 * @example
 *   f(1, 2, 3)                              → callee is IdentifierExprAST
 *   Buffer<int>(capacity)                   → genericArgs = [Int]
 *   math:sqrt(x)                            → callee is ModuleAccessExprAST
 *   x |> map<int, string>(stringFromInt)!   → hasArgPack = true
 *
 * ─── Generic Instantiation ──────────────────────────────────────────────────
 * Generic arguments are stored in `genericArgs`. The callee remains a plain
 * function reference; the generic arguments are applied at the call site.
 *
 * ─── Argument Pack (!) ──────────────────────────────────────────────────────
 * `fn(args)!` is not a function call – `!` marks an intentionally incomplete
 * argument list. The upstream value is injected as the **first** argument when
 * `|>` fires. The semantic pass verifies that `hasArgPack` is only true when
 * the call is inside a pipeline step.
 *
 * ─── Return Type Handling ──────────────────────────────────────────────────
 * For functions returning multiple values, the call site destructures the
 * result into multiple variables: `let value int, ok bool = parseInt("42")`
 *
 * @field callee        The function being called.
 * @field genericArgs   Generic arguments (empty if none).
 * @field args          Call arguments.
 * @field hasArgPack    True if this is `fn(args)!` (argument pack for pipeline).
 */
struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    ExprPtr callee;
    ArenaSpan<TypePtr> genericArgs;
    ArenaSpan<ExprPtr> args;
    bool hasArgPack = false;    // true for `fn(args)!`

    CallExprAST() : ExprAST(ASTKind::CallExpr) {}
};

/**
 * @brief Array element access.
 *
 * @example
 *   nums[2]      → index = 2
 *
 * The index is runtime-checked. Out-of-bounds access panics unless guarded
 * with `??`: `nums[i] ?? 0`
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Runtime Check**: Indexing a slice (`[_]T`) or dynamic array (`[*]T`)
 *    is always runtime-checked. A literal index does not prove in-bounds
 *    against a slice of unknown length.
 * 2. **Compile-Time Check**: Indexing a fixed-size array (`[N]T`) with a
 *    literal index that is provably less than `N` is checked at compile time.
 * 3. **Panic Handling**: Out-of-bounds access panics unless guarded with `??`.
 * 4. **Type**: The result type is the element type of the array.
 *
 * @field target        The array being indexed.
 * @field index         The index expression (must be integer type).
 */
struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    ExprPtr target;
    ExprPtr index;      // element index

    IndexExprAST() : ExprAST(ASTKind::IndexExpr) {}
};

/**
 * @brief Slice expression – produces a borrowed view over a contiguous range.
 *
 * @example
 *   nums[1..3]   → start = 1, end = 3,   isExclusive = false
 *   nums[1..<3]  → start = 1, end = 3,   isExclusive = true  (end excluded)
 *   nums[..<2]   → start = nullptr, end = 2, isExclusive = true
 *   nums[3..]    → start = 3, end = nullptr, isExclusive = false
 *   nums[..]     → start = nullptr, end = nullptr, isExclusive = false
 *
 * ─── Slice Rules ────────────────────────────────────────────────────────────
 * 1. **Borrowed View**: A slice `[_]T` is a borrowed view – it does not own
 *    the underlying memory. The backing array must outlive the slice.
 * 2. **Bounds**: Start defaults to 0, end defaults to the array's length.
 * 3. **Runtime Check**: Slice bounds are runtime-checked. Out-of-bounds
 *    access panics unless guarded with `??`.
 * 4. **Inclusive/Exclusive**: `..` is inclusive, `..<` is exclusive.
 *
 * @field target        The array being sliced.
 * @field start         Inclusive start (nullptr means 0).
 * @field end           End bound (nullptr means array length).
 * @field isExclusive   True for `..<` syntax (end is exclusive).
 */
struct SliceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::SliceExpr;

    ExprPtr target;
    ExprPtr start;      // inclusive, nullptr means 0
    ExprPtr end;        // inclusive or exclusive depending on isExclusive
    bool isExclusive = false; // true for ..< syntax

    SliceExprAST() : ExprAST(ASTKind::SliceExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// OPERATOR NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An infix binary operation.
 *
 * @example
 *   a + b    → op = Add
 *   x == y   → op = Eq
 *   p and q  → op = And (short‑circuit, logical)
 *   a & b    → op = BitAnd (bitwise AND, integer types only)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Logical Operators**: `and` and `or` are short-circuiting and accept
 *    any type (coerced to bool). Result is always bool.
 * 2. **Bitwise Operators**: `&`, `|`, `^`, `<<`, `>>` are integer-only.
 * 3. **Comparison**: `==` and `!=` compare values. Reference equality is
 *    not a separate operator (use `&` and compare addresses).
 * 4. **Arithmetic**: `+`, `-`, `*`, `/`, `%`, `**` are numeric-only.
 */
struct BinaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BinaryExpr;

    BinaryOp op;
    ExprPtr left;
    ExprPtr right;

    BinaryExprAST() : ExprAST(ASTKind::BinaryExpr) {}
};

/**
 * @brief A prefix unary operation.
 *
 * @example
 *   -x      → op = Neg
 *   not x   → op = Not
 *   ~x      → op = BitNot
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Ref Operator**: `&x` takes a reference to `x`. `x` must be an lvalue.
 *    The result type is `&T` where `T` is the type of `x`.
 * 2. **Bitwise NOT**: `~` is integer-only.
 * 3. **Logical NOT**: `not` accepts any type (coerced to bool). Result is bool.
 * 4. **Arithmetic Negation**: `-` is numeric-only.
 */
struct UnaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnaryExpr;

    UnaryOp op;
    ExprPtr operand;

    UnaryExprAST() : ExprAST(ASTKind::UnaryExpr) {}
};

/**
 * @brief An assignment – plain or compound.
 *
 * @example
 *   x = 5     → op = Assign
 *   x += 1    → op = AddAssign (desugars to x = x + 1)
 *
 * Compound operators desugar to `lhs = lhs op rhs` at semantic time.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Lvalue Required**: The left-hand side must be an assignable lvalue
 *    (variable, field access, or index expression).
 * 2. **Const Checking**: Assigning to a `const` variable or `const` field
 *    is a semantic error.
 * 3. **Type Matching**: The right-hand side type must match the left-hand
 *    side type.
 */
struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    AssignOp op;
    ExprPtr lhs;
    ExprPtr rhs;

    AssignExprAST() : ExprAST(ASTKind::AssignExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NULLABLE NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A ?. chain – each step is only evaluated if the previous value is non‑nil.
 *
 * @example
 *   player?.weapon?.damage ?? 0
 *
 * The grammar enforces that every ?. chain MUST be terminated by ??.
 * Standalone '.' becomes FieldAccessExprAST.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Short-Circuit**: Each step is only evaluated if the previous value
 *    is non-nil. If any step is nil, the chain short-circuits to nil.
 * 2. **Termination Required**: Every ?. chain must be terminated by `??`.
 * 3. **Type**: The result type is the type of the final field, or nullable
 *    if the chain ends in nil.
 *
 * @field object        The root expression before the first ?.
 * @field steps         The field names accessed via ?. in order.
 */
struct NullableChainExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullableChainExpr;

    ExprPtr object;
    ArenaSpan<InternedString> steps;   // field names accessed via ?.

    NullableChainExprAST() : ExprAST(ASTKind::NullableChainExpr) {}
};

/**
 * @brief The null coalescing operator – provides a fallback value when the LHS is nil or err.
 *
 * @example
 *   value ?? fallback
 *   riskyOp() ?? -1
 *   lookup() ?? User { id = 0, name = "guest" }
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Sentinel Coverage**: `??` triggers when the left-hand side is `nil`,
 *    `err`, or both (for `T?!` types).
 * 2. **Result Type**: The result type is whatever type `rhs` produces,
 *    checked against `lhs`'s own type.
 * 3. **Block Form**: The right-hand side may be a block, which can re-raise
 *    `err` instead of fully resolving the value.
 *
 * @field value          The nullable/fallible value.
 * @field fallback       The fallback expression.
 */
struct NullCoalesceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullCoalesceExpr;

    ExprPtr value;
    ExprPtr fallback;

    NullCoalesceExprAST() : ExprAST(ASTKind::NullCoalesceExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// PIPELINE & COMPOSITION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief One step in a pipeline chain – owned by PipelineExprAST.
 *
 * Grammar: pipeline_step := expr [ '(' arg_list ')' '!' ] | func_literal
 *
 * The `callable` expression is the result of `parseFuncRef()` or an anonymous
 * function expression. If the step includes an argument pack `(args)!`, then
 * `packArgs` is non‑empty and the step is an argument pack step (the `!`
 * annotation).
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Argument Pack**: `!` marks an intentionally incomplete argument list.
 *    The upstream value is injected as the first argument when `|>` fires.
 * 2. **Curried Functions**: `|>` fills exactly one parameter group. A curried
 *    function with remaining unfilled groups is a compile error.
 * 3. **Generic Functions**: Generic functions must be instantiated with
 *    explicit type arguments at the pipeline step site.
 * 4. **Nullable/Fallible Steps**: A `~[nullable]` or `~[fallible]` function
 *    is forbidden as a pipeline step – the pipeline has no way to narrow it.
 *
 * @field callable       The function reference or anonymous function.
 * @field packArgs       Non‑empty for argument pack steps (the `!` annotation).
 */
struct PipelineStepAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineStep;

    ExprPtr callable;                // function reference or anonymous function
    ArenaSpan<ExprPtr> packArgs;     // non‑empty for argument pack steps

    PipelineStepAST() : BaseAST(ASTKind::PipelineStep) {}
};
using PipelineStepPtr = PipelineStepAST*;

/**
 * @brief A runtime pipeline chain – seed |> step |> step |> ...
 *
 * @example
 *   42 |> float |> sqrt
 *   getUser(id) |> validate |> save
 *   v |> Vec2:normalize |> scale(2.0)!
 *
 * The pipeline short‑circuits on Error when the error library is used.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Left-to-Right**: Pipeline executes left to right at runtime.
 * 2. **Argument Injection**: Each step's upstream value is injected as the
 *    first argument when `|>` fires.
 * 3. **Type Chaining**: The output type of each step must match the input
 *    type of the next step.
 * 4. **Short-Circuit**: Pipelines short-circuit on Error when using the
 *    error library.
 *
 * @field seed           The initial value.
 * @field steps          Pipeline steps in order (at least one).
 */
struct PipelineExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineExpr;

    ExprPtr seed;
    ArenaSpan<PipelineStepPtr> steps;   // at least one

    PipelineExprAST() : ExprAST(ASTKind::PipelineExpr) {}
};

/**
 * @brief One operand in a +> composition chain – owned by ComposeExprAST.
 *
 * Grammar (from LUCID_GRAMMAR.md):
 *   compose_operand := expr                          -- function reference
 *
 * The callable expression can be:
 *   - IdentifierExprAST (plain function name)
 *   - FieldAccessExprAST (dotted path)
 *   - ModuleAccessExprAST (module:function)
 *
 * Generic arguments are applied to the callable (e.g., `toString<int>` becomes
 * callable = IdentifierExprAST("toString") with genericArgs = [int]).
 *
 * ─── Composition Rules ──────────────────────────────────────────────────────
 * 1. **Single Parameter Group**: Both operands must have exactly one
 *    parameter group. Curry functions are forbidden on either side.
 * 2. **Type Matching**: The output type of the left operand must exactly
 *    match the input type of the right operand.
 * 3. **Generic Instantiation**: Generic functions must be instantiated
 *    with explicit type arguments before composition.
 * 4. **Nullable/Fallible Forbidden**: `~[nullable]` and `~[fallible]`
 *    functions are forbidden as composition operands.
 * 5. **Async Composition**: When any operand is `~[async]`, the composed
 *    function must be declared `~[async]` and awaited at the call site.
 *
 * @field callable       The function reference (required).
 * @field genericArgs    Explicit type arguments for generic instantiation.
 */
struct ComposeOperandAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeOperand;

    ExprPtr callable;                    // the function reference (required)
    ArenaSpan<TypePtr> genericArgs;      // explicit type arguments for generic instantiation

    ComposeOperandAST() : BaseAST(ASTKind::ComposeOperand) {}
};
using ComposeOperandPtr = ComposeOperandAST*;

/**
 * @brief A compile‑time function composition chain – f +> g +> h
 *
 * Grammar (from LUCID_GRAMMAR.md):
 *   compose_expr := expr { '+>' compose_operand }
 *
 * @example
 *   const process = validate +> transform +> render
 *   const intToString = identity<int> +> toString<int> +> trim
 *
 * ─── Key Characteristics ──────────────────────────────────────────────────
 * - Compile-time: Produces a new function without executing anything.
 * - Type Matching: Strict – output type of left must exactly match input type of right.
 * - No Qualifiers: `~[async]` or `~[nullable]` operands are forbidden.
 * - Generic Instantiation: Explicit type arguments required for generic functions.
 *
 * @field left           The leftmost operand.
 * @field operands       Right‑hand operands in order (at least one).
 */
struct ComposeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeExpr;

    ExprPtr left;                                // leftmost operand (already parsed)
    ArenaSpan<ComposeOperandPtr> operands;        // right‑hand operands in order

    ComposeExprAST() : ExprAST(ASTKind::ComposeExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FUNCTION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An anonymous function expression – a function value without a name.
 *
 * @example
 *   (x int) -> int { return x * 2 }
 *   (a int)(b int) -> int { return a + b }   – curried (Form 2)
 *
 * The parser desugars Form 2 `()()` shorthand into nested Form 1 functions
 * before building the AST. The `funcType` captures the full curried structure.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Form 1**: Explicit intermediate `->` return – code runs between groups.
 * 2. **Form 2**: `()()` shorthand – compiler expands to nested Form 1 functions.
 * 3. **Type**: The anonymous function's type is captured in `funcType`.
 *
 * @field funcType       The anonymous function type (may be curried).
 * @field body           Function body (always BlockStmtAST).
 */
struct AnonFuncExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AnonFuncExpr;

    FuncTypeAST* funcType = nullptr;   // the anonymous function type
    StmtPtr body = nullptr;           // always BlockStmtAST

    bool hasParams() const { return funcType && !funcType->params.empty(); }

    AnonFuncExprAST() : ExprAST(ASTKind::AnonFuncExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONTROL FLOW EXPRESSION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief The expression form of if – both branches required, both produce the same type.
 *
 * @example
 *   if score >= 60 ?? "pass" else "fail"
 *   if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
 *
 * Grammar: if_expr := 'if' expr '??' expr 'else' expr
 *
 * This is distinct from IfStmtAST (in StmtAST.hpp) where else is optional
 * and no value is produced.
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Else Required**: Both branches must be present.
 * 2. **Type Matching**: Both branches must produce compatible types.
 * 3. **Chaining**: Chained if-expressions are right-associative.
 *
 * @field condition       The condition expression.
 * @field thenBranch      The then branch (expression).
 * @field elseBranch      The else branch (expression).
 */
struct IfExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IfExpr;

    ExprPtr condition;
    ExprPtr thenBranch; // expression
    ExprPtr elseBranch; // expression

    IfExprAST() : ExprAST(ASTKind::IfExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// OTHER EXPRESSION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An inclusive range literal – lo..hi or lo..<hi.
 *
 * @example
 *   0..10   – used in for loops: for i int in 0..10
 *   1..10   – used in match range patterns: case 1..10
 *   1..3    – used in slice index: nums[1..3]
 *
 * Both ends are inclusive for '..' – 0..10 iterates 0,1,2,...,10 (11 steps).
 * When isExclusive is true, the end is exclusive (..< syntax).
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * 1. **Range Positions**: A range only appears in three positions:
 *    - Range iteration in `for`
 *    - Slice bounds
 *    - `switch` case values
 * 2. **No Standalone Type**: A range is not a standalone collection value
 *    with its own type – there is no general-purpose range type.
 * 3. **Literal Bounds**: In `switch` cases, both bounds must be literals.
 *
 * @field lo             Start (inclusive).
 * @field hi             End (inclusive/exclusive depends on flag).
 * @field isExclusive    True for `..<` syntax (end is exclusive).
 * @note start and end must always be a positive integer
 */
struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprPtr lo;   // start (inclusive)
    ExprPtr hi;   // end (inclusive/exclusive depends on flag)
    bool isExclusive = false;   // true for ..<

    RangeExprAST() : ExprAST(ASTKind::RangeExpr) {}
};

/**
 * @brief A compiler‑builtin call invoked with the '#' prefix.
 *
 * @example
 *   #sizeof(T)      – compile‑time size of a type in bytes
 *   #memcpy(d,s,l)  – memory copy intrinsic
 *   #sqrt(x)        – hardware‑accelerated sqrt
 *
 * The semantic pass validates arguments and sets resolvedType.
 * Codegen maps intrinsicName to the corresponding intrinsic operation.
 *
 * @field intrinsicName  The intrinsic name ("sizeof", "memcpy", "sqrt", etc.).
 * @field args           Value arguments in order.
 */
struct IntrinsicCallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IntrinsicCallExpr;

    InternedString intrinsicName; // "sizeof", "memcpy", "sqrt", etc.
    ArenaSpan<ExprPtr> args;      // value arguments in order

    IntrinsicCallExprAST() : ExprAST(ASTKind::IntrinsicCallExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Aliases for common pointer types.
// ─────────────────────────────────────────────────────────────────────────────

using LiteralExprPtr = LiteralExprAST*;
using ArrayLiteralExprPtr = ArrayLiteralExprAST*;
using StructLiteralExprPtr = StructLiteralExprAST*;
using FieldInitPtr = FieldInitAST*;
using IdentifierExprPtr = IdentifierExprAST*;
using FieldAccessExprPtr = FieldAccessExprAST*;
using ModuleAccessExprPtr = ModuleAccessExprAST*;
using CallExprPtr = CallExprAST*;
using IndexExprPtr = IndexExprAST*;
using SliceExprPtr = SliceExprAST*;
using BinaryExprPtr = BinaryExprAST*;
using UnaryExprPtr = UnaryExprAST*;
using AssignExprPtr = AssignExprAST*;
using NullableChainExprPtr = NullableChainExprAST*;
using NullCoalesceExprPtr = NullCoalesceExprAST*;
using PipelineStepPtr = PipelineStepAST*;
using PipelineExprPtr = PipelineExprAST*;
using ComposeOperandPtr = ComposeOperandAST*;
using ComposeExprPtr = ComposeExprAST*;
using AnonFuncExprPtr = AnonFuncExprAST*;
using IfExprPtr = IfExprAST*;
using RangeExprPtr = RangeExprAST*;
using IntrinsicCallExprPtr = IntrinsicCallExprAST*;