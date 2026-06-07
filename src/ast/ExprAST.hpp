/**
 * @file ExprAST.hpp
 *
 * @responsibility Defines all AST nodes that evaluate to a value – literals,
 *                 operations, calls, control flow expressions, and patterns.
 *
 * @hierarchy BaseAST → ExprAST → [Concrete Expression Nodes]
 *            BaseAST → PatternAST → [Concrete Pattern Nodes]
 *
 * @related_files
 *   - src/parser/ParserExpr.cpp – primary producer of expression nodes
 *   - src/ast/StmtAST.hpp – statements that contain expressions
 *   - src/semantic/TypeChecker.cpp – consumes for type validation
 *
 * @note Pattern nodes are separate from expression nodes (PatternAST vs ExprAST)
 *       because patterns have different semantics and are only valid in match
 *       contexts. PatternExprAST wraps LiteralExprAST and RangeExprAST for
 *       uniform pattern handling.
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"

#include <string>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// LiteralKind – discriminator for literal expressions.
// ─────────────────────────────────────────────────────────────────────────────

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
    RawString,  // r"raw\nno escaping"
    Char,       // 'a'
    Hex,        // 0xFF
    Binary,     // 0b1010
    True,       // true
    False,      // false
    Nil,        // nil
};

// ─────────────────────────────────────────────────────────────────────────────
// AssignOp – assignment operators.
// ─────────────────────────────────────────────────────────────────────────────

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
    PowAssign,    // ^=
    ModAssign,    // %=
    BitAndAssign, // &&=
    BitOrAssign,  // ||=
    BitXorAssign, // ~^=
    ShlAssign,    // <<=
    ShrAssign,    // >>=
};

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOp – infix binary operators.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Identifies the binary operator in a BinaryExprAST.
 *
 * The parser maps token(s) to this enum before constructing the node.
 *
 * @note '&' is the unary reference operator – never a binary op.
 *       Bitwise AND uses '&&' (BIT_AND token), bitwise OR uses '||' (BIT_OR token).
 */
enum class BinaryOp {
    // Arithmetic
    Add,  // +
    Sub,  // -
    Mul,  // *
    Div,  // /
    Pow,  // ^
    Mod,  // %

    // Comparison – value equality
    Eq,     // ==
    Ne,     // !=
    Lt,     // <
    Gt,     // >
    Le,     // <=
    Ge,     // >=
    RefEq,  // === (reference equality – same memory address)

    // Logical (short‑circuit)
    And,  // and
    Or,   // or

    // Bitwise (integer types only)
    BitAnd,  // &&
    BitOr,   // ||
    BitXor,  // ~^
    Shl,     // <<
    Shr,     // >>
};

// ─────────────────────────────────────────────────────────────────────────────
// UnaryOp – prefix unary operators.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Identifies the unary operator in a UnaryExprAST.
 */
enum class UnaryOp {
    Neg,    // -x       arithmetic negation
    Not,    // not x    logical negation
    BitNot, // ~~x       bitwise NOT
    Ref,    // &x       take a reference
};

// ─────────────────────────────────────────────────────────────────────────────
// CallKind – distinguishes the kind of function call
// ─────────────────────────────────────────────────────────────────────────────

enum class CallKind {
    Plain,      // normal function call
    Async,      // ~async – must be awaited
    Nullable,   // ~nullable – emit warning, nil check needed
    Parallel    // ~parallel – handled by codegen
};

// ─────────────────────────────────────────────────────────────────────────────
// ComposeOperandKind – syntactic form of a compose operand.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Distinguishes the syntactic forms a compose operand can take.
 *
 * Mirrors PipelineStepKind but without ArgPack (! is only valid in pipelines).
 */
enum class ComposeOperandKind {
    Ident,       // fn
    BehaviorRef, // obj:method
    FieldRef,    // obj.field – non‑nullable only
};

// ─────────────────────────────────────────────────────────────────────────────
// LITERAL NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A scalar literal value – numbers, strings, characters, booleans, nil.
 *
 * @example
 *   42         → kind=Int,    value="42"
 *   3.14       → kind=Float,  value="3.14"
 *   "hello"    → kind=String, value="hello"
 *   r"a\nb"    → kind=RawString, value="a\\nb"
 *   'A'        → kind=Char,   value="A"
 *   0xFF       → kind=Hex,    value="0xFF"
 *   true       → kind=True,   value="true"
 *   nil        → kind=Nil,    value="nil"
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
 */
struct ArrayLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayLiteralExpr;

    ArenaSpan<ExprAST*> elements; // may be empty

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
    ExprAST* value;      // initialiser expression

    FieldInitAST() : BaseAST(ASTKind::FieldInit) {}
    FieldInitAST(InternedString n, ExprAST* v)
        : BaseAST(ASTKind::FieldInit), name(n), value(v) {}

};

using FieldInitPtr = FieldInitAST*;

/**
 * @brief Constructs a value of a named struct type.
 *
 * @example
 *   Vec2 { x = 1.0, y = 2.0 }
 *   Color {}  – all fields take their defaults
 *   Pair<int, string> { first = 1, second = "one" }
 *
 * Field inits always use '=', never ':'. The ':' only appears inside match
 * struct patterns (handled by StructPatternAST).
 *
 * genericArgs is populated when the type is explicitly instantiated.
 * inits maps field name → initialiser expression (order not significant).
 */
struct StructLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::StructLiteralExpr;

    InternedString typeName;               // "Vec2", "Color", "Pair"
    ArenaSpan<TypeAST*> genericArgs;       // empty if non‑generic
    ArenaSpan<FieldInitPtr> inits;         // field = expr entries
    NamedTypeAST* instantiatedType;        // semantic cache (raw pointer)

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
 */
struct IdentifierExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IdentifierExpr;

    InternedString name;
    ArenaSpan<TypeAST*> genericArgs;

    explicit IdentifierExprAST(InternedString n)
        : ExprAST(ASTKind::IdentifierExpr), name(n) {}
};

/**
 * @brief Accesses a data member (struct field, enum variant, or module path) via '.' operator.
 *
 * @example
 *   v.x                     → object = identifier("v"), field = "x"
 *   Direction.North         → object = identifier("Direction"), field = "North"
 *   math.utils.toString     → object = identifier("math"), field = "utils" (then nested)
 *
 * When the field refers to a generic function (e.g., `math.utils.toString<int>`),
 * the FieldAccessExprAST becomes the `entity` of a GenericInstantiationExprAST.
 */
struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    ExprAST* object;
    InternedString field;
    ArenaSpan<TypeAST*> genericArgs; // Generic function

    FieldAccessExprAST() : ExprAST(ASTKind::FieldAccessExpr) {}
};

/**
 * @brief Accesses a method on a value via the ':' operator.
 *
 * @example
 *   v:normalize        → object = identifier("v"), method = "normalize"
 *   getValue():length  → object = callExpr, method = "length"
 *   (point + offset):normalize → object = binaryExpr, method = "normalize"
 *
 * This node represents a METHOD REFERENCE, not a call.
 * For a method call, this node becomes the callee of a CallExprAST.
 *
 * IMPORTANT: Method references CANNOT have explicit generic arguments.
 * Generic type parameters are determined entirely by the receiver type.
 * For example, if `list` is `List<int>`, then `list:map` automatically
 * knows that T = int.
 *
 * To call a generic function in a module path, use FieldAccessExprAST:
 *   utils.toString<int>(value)  ← FieldAccessExprAST with genericArgs
 */
struct BehaviorAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BehaviorAccessExpr;

    ExprAST* object;          ///< The receiver expression
    InternedString method;    ///< The method name (no generic args)

    BehaviorAccessExprAST() : ExprAST(ASTKind::BehaviorAccessExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CALL & INDEX NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A function call – supports regular calls, method calls, array element calls,
 *        generic instantiation, argument pack (!), and async detection.
 *
 * @par Grammar Reference (from LUC_GRAMMAR.md)
 *   postfix_op := '(' [ arg_list ] ')'                -- regular call
 *               | '(' [ arg_list ] ')' '!'            -- argument pack (pipeline injection)
 *               | generic_args '(' [ arg_list ] ')'   -- generic instantiation
 *
 * The callable can be:
 *   - A bare identifier:        `f(args)`
 *   - A method on a value:      `obj:method(args)`
 *   - A field access:           `obj.field(args)`
 *   - An array element:         `arr[idx](args)`
 *   - Any expression that yields a function type
 *
 * @example
 *   f(1, 2, 3)                              → callee is IdentifierExprAST
 *   Buffer<int>(capacity)                   → genericArgs = [Int]
 *   obj:method(a, b)                        → callee is BehaviorAccessExprAST
 *   arr[idx](x, y)                          → callee is IndexExprAST
 *   utils.dosomething()                     → callee is FieldAccessExprAST
 * 
 * @note 
 *  slice[1..5]() is not valid
 *  obj:dosomething<T>() - wrong method does not have independent generic
 *
 * @par Generic Instantiation
 *   Generic arguments are stored in `genericArgs`. The callee remains a plain
 *   function reference; the generic arguments are applied at the call site.
 *
 * @par Async Calls
 *   `isAsyncCall` is set by the semantic pass after resolving the callee.
 *   If the callee's type has the `~async` qualifier, this flag is true and
 *   the call must be preceded by `await` (unless inside another async function).
 *
 */
struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    ExprAST* callee;
    ArenaSpan<TypeAST*> genericArgs;
    ArenaSpan<ExprAST*> args;
    CallKind callKind = CallKind::Plain;

    CallExprAST() : ExprAST(ASTKind::CallExpr) {}
};


/**
 * @brief Array element access or slice expression.
 *
 * @example
 *   nums[2]      → kind = Element, index = 2, sliceEnd = nullptr
 */
struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    ExprAST* target;
    ExprAST* index;      // element index

    IndexExprAST() : ExprAST(ASTKind::IndexExpr) {}
};

/**
 * @brief Array element access or slice expression.
 *
 * @example
 *   nums[1..3]   → kind = Slice,   index = 1, sliceEnd = 3
 */
struct SliceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::SliceExpr;

    ExprAST* target;
    ExprAST* start;      // inclusive, nullptr means 0
    ExprAST* end;        // inclusive or exclusive depending on isExclusive
    bool isExclusive = false; // true for ..< syntax

    // Semantic cache: the resulting slice type
    mutable TypeAST* sliceType = nullptr;

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
 *   p and q  → op = And (short‑circuit)
 *   a && b   → op = BitAnd (bitwise AND, integer types only)
 */
struct BinaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BinaryExpr;

    BinaryOp op;
    ExprAST* left;
    ExprAST* right;

    BinaryExprAST() : ExprAST(ASTKind::BinaryExpr) {}
};

/**
 * @brief A prefix unary operation.
 *
 * @example
 *   -x      → op = Neg
 *   not x   → op = Not
 *   ~~x     → op = BitNot
 *   &x      → op = Ref (take a reference)
 */
struct UnaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnaryExpr;

    UnaryOp op;
    ExprAST* operand;

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
 */
struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    AssignOp op;
    ExprAST* lhs;
    ExprAST* rhs;

    AssignExprAST() : ExprAST(ASTKind::AssignExpr) {}
};

/**
 * @brief Runtime type check – produces bool and narrows type in enclosing block.
 *
 * @example
 *   x is int
 *   shape is Circle
 *   stage is Fragment
 *
 * After a successful `if x is SomeType { ... }`, x is treated as SomeType
 * inside the block.
 */
struct IsExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IsExpr;

    ExprAST* expr;
    TypeAST* checkType;   // the type being checked against

    IsExprAST() : ExprAST(ASTKind::IsExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NULLABLE CHAIN NODES
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
 * object – the root expression before the first ?.
 * steps  – the field names accessed via ?. in order
 */
struct NullableChainExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullableChainExpr;

    ExprAST* object;
    ArenaSpan<InternedString> steps;   // field names accessed via ?.

    NullableChainExprAST() : ExprAST(ASTKind::NullableChainExpr) {}
};

/**
 * @brief The null coalescing operator – provides a fallback value when the LHS is nil.
 *
 * @example
 *   value ?? fallback
 */
struct NullCoalesceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullCoalesceExpr;

    ExprAST* value;
    ExprAST* fallback;

    NullCoalesceExprAST() : ExprAST(ASTKind::NullCoalesceExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// PIPELINE & COMPOSITION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief One step in a pipeline chain – owned by PipelineExprAST.
 *
 * Grammar: pipeline_step := func_ref [ '(' arg_list ')' '!' ] | anon_func
 *
 * The `callable` expression is the result of `parseFuncRef()` or an anonymous
 * function expression. If the step includes an argument pack `(args)!`, then
 * `packArgs` is non‑empty and the step is an argument pack step (the `!`
 * annotation). The `kind` field is kept for quick classification but can be
 * derived from the callable.
 */
struct PipelineStepAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineStep;

    ExprAST* callable;                // function reference or anonymous function
    ArenaSpan<ExprAST*> packArgs;     // non‑empty for argument pack steps

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
 */
struct PipelineExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineExpr;

    ExprAST* seed;
    ArenaSpan<PipelineStepPtr> steps;   // at least one

    PipelineExprAST() : ExprAST(ASTKind::PipelineExpr) {}
};

/**
 * @brief One operand in a +> composition chain – owned by ComposeExprAST.
 *
 * Grammar (from LUC_GRAMMAR.md):
 *   compose_operand := func_ref                     -- named function, module path
 *                    | expr ':' IDENTIFIER          -- method reference on a value
 *                    | expr '.' IDENTIFIER          -- non‑nullable data field only
 *                    | generic_args compose_operand -- generic instantiation (e.g., toString<int>)
 *
 * The callable expression can be:
 *   - IdentifierExprAST (plain function name)
 *   - FieldAccessExprAST (dotted path)
 *   - BehaviorAccessExprAST (method reference)
 *
 * Generic arguments are applied to the callable (e.g., `toString<int>` becomes
 * callable = IdentifierExprAST("toString") with genericArgs = [int]).
 *
 * @note No argument pack or anonymous function in composition operands.
 * @note ~nullable function as operand is forbidden.
 */
struct ComposeOperandAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeOperand;

    ExprAST* callable;                    // the function reference (required)
    ArenaSpan<TypeAST*> genericArgs;      // explicit type arguments for generic instantiation

    ComposeOperandAST() : BaseAST(ASTKind::ComposeOperand) {}
};

using ComposeOperandPtr = ComposeOperandAST*;

/**
 * @brief A compile‑time function composition chain – f +> g +> h
 *
 * Grammar (from LUC_GRAMMAR.md):
 *   compose_expr := pipeline_expr { '+>' compose_operand }
 *
 * @example
 *   let process = validate +> transform +> render
 *   let intToString = identity<int> +> toString<int> +> trim
 *
 * +> enforces strict type matching at compile time: the output type of the
 * left operand must exactly match the input type of the next operand.
 *
 * ─── Qualifier Rules ──────────────────────────────────────────────────────
 *   - If any operand has ~async or ~nullable, the composition is forbidden
 *     (qualifiers must be handled before composition).
 *   - The resulting function is plain (no qualifiers).
 *   - Assign the qualifier to the binding that holds the composed result.
 *
 * ─── Generic Functions ────────────────────────────────────────────────────
 *   - Generic functions must be instantiated with explicit type arguments
 *     before composition (e.g., `toString<int>`).
 *   - Uninstantiated generic functions are not valid operands.
 */
struct ComposeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeExpr;

    ExprAST* left;                                // leftmost operand (already parsed)
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
 *   (x int) int { return x * 2 }
 *   (a int) (b int) int { return a + b }   – curried
 *
 */
struct AnonFuncExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AnonFuncExpr;

    FuncTypeAST* funcType = nullptr;   // the anonymous function type
    StmtAST* body = nullptr;           // always BlockStmtAST

    bool hasParams() const { return funcType && !funcType->params.empty(); }

    AnonFuncExprAST() : ExprAST(ASTKind::AnonFuncExpr) {}
};

/**
 * @brief Suspends the current async function until the awaited future resolves.
 *
 * @example
 *   await httpGet(url)
 *   await fetchAll(items)
 *
 * Only valid inside an async function body – the semantic pass reports an
 * error if await appears outside of one.
 */
struct AwaitExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AwaitExpr;

    ExprAST* inner;   // the async expression being awaited

    explicit AwaitExprAST(ExprAST* e)
        : ExprAST(ASTKind::AwaitExpr), inner(e) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// RESOLVE NODES — structured error unwrapping
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief The `ok` arm of a resolve expression.
 *
 * @example
 *   ok (v int)    { return v }
 *   ok (v int?)   { return v ?? 0 }
 *
 * Grammar: 'ok' '(' IDENTIFIER type ')' block
 *
 * `bindType` is always plain T — never T!E. The `!` is consumed at the
 * resolve boundary; the ok arm receives the already-unwrapped success value.
 *
 * Extends BaseAST (not StmtAST) — mirrors MatchArmAST / DefaultArmAST.
 */
struct OkArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::OkArm;

    InternedString bindName;   ///< Name of the success variable (e.g. "v")
    TypeAST* bindType;         ///< Plain T (never T!E — ! is consumed by resolve)
    StmtAST* body;             ///< Always BlockStmtAST

    OkArmAST() : BaseAST(ASTKind::OkArm) {}
};

using OkArmPtr = OkArmAST*;

/**
 * @brief The `err` arm of a resolve expression.
 *
 * @example
 *   err (e string) { return -1 }    -- typed error: E = string
 *   err ()         { return 0  }    -- bare '!': no error payload
 *
 * Grammar: 'err' '(' [ IDENTIFIER type ] ')' block
 *
 * When the result type used bare `!` (no error type), the parens are empty:
 *   `bindName` is empty string, `bindType` is nullptr.
 *
 * Extends BaseAST (not StmtAST) — mirrors MatchArmAST / DefaultArmAST.
 */
struct ErrArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::ErrArm;

    InternedString bindName;   ///< Error variable name; empty string when bare '!'
    TypeAST* bindType;         ///< Error type E; nullptr when bare '!' (no error value)
    StmtAST* body;             ///< Always BlockStmtAST

    /// True when the enclosing result type was bare '!' (no error payload)
    bool isBareError() const { return bindType == nullptr; }

    ErrArmAST() : BaseAST(ASTKind::ErrArm) {}
};

using ErrArmPtr = ErrArmAST*;

/**
 * @brief Structured resolution of a T!E value — forces handling of both outcomes.
 *
 * @example
 *   resolve divide(10, 0) {
 *       ok  (v int)    { return v  }
 *       err (e string) { return -1 }
 *   }
 *
 * Grammar: 'resolve' expr '{' ok_arm err_arm '}'
 *
 * The `subject` must resolve to a T!E type. After the resolve block, the `!`
 * is consumed and the result is plain T (the type returned by the ok arm).
 * Both arms are required; both must return the same type.
 *
 * Listed in `primary_expr` in the grammar — this is an expression, not a
 * statement, exactly like MatchExprAST.
 */
struct ResolveExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ResolveExpr;

    ExprAST* subject;    // The T!E expression being resolved
    OkArmPtr   okArm;    // Required ok arm
    ErrArmPtr  errArm;   // Required err arm

    ResolveExprAST() : ExprAST(ASTKind::ResolveExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONTROL FLOW EXPRESSION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief The expression form of if – both branches required, both produce the same type.
 *
 * @example
 *   if n >= 0 ?? "positive" else "negative"
 *
 * The grammar is: if_expr := 'if' expr '??' expr 'else' expr
 *
 * This is distinct from IfStmtAST (in StmtAST.hpp) where else is optional
 * and no value is produced.
 */
struct IfExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IfExpr;

    ExprAST* condition;
    ExprAST* thenBranch; // expression
    ExprAST* elseBranch; // expression

    IfExprAST() : ExprAST(ASTKind::IfExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// OTHER EXPRESSION NODES
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An inclusive range literal – lo..hi or lo..<hi.
 *
 * @example
 *   0..10   – used in for loops: for i in 0..10
 *   1..10   – used in match range patterns: case 1..10
 *   1..3    – used in slice index: nums[1..3]
 *
 * Both ends are inclusive for '..' – 0..10 iterates 0,1,2,...,10 (11 steps).
 * When isExclusive is true, the end is exclusive (..< syntax).
 */
struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprAST* lo;   // start (inclusive)
    ExprAST* hi;   // end (inclusive/exclusive depends on flag)
    bool isExclusive = false;   // true for ..<

    RangeExprAST() : ExprAST(ASTKind::RangeExpr) {}
};

/**
 * @brief A compiler‑builtin call invoked with the '#' prefix.
 *
 * @example
 *   #sizeof(T)      – compile‑time size of a type in bytes
 *   #memcpy(d,s,l)  – LLVM memcpy intrinsic
 *   #sqrt(x)        – hardware‑accelerated sqrt
 *
 * The semantic pass validates arguments and sets resolvedType.
 * Codegen maps intrinsicName to the corresponding LLVM intrinsic ID.
 */
struct IntrinsicCallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IntrinsicCallExpr;

    InternedString intrinsicName; // "sizeof", "memcpy", "sqrt", etc.
    ArenaSpan<ExprAST*> args;      // value arguments in order

    IntrinsicCallExprAST() : ExprAST(ASTKind::IntrinsicCallExpr) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// PATTERN NODES
// ─────────────────────────────────────────────────────────────────────────────
//
// Pattern nodes are used exclusively in match expressions. They are separate
// from ExprAST because patterns have different semantics (binding, destructuring)
// and are only valid in match contexts.
//
// PatternExprAST wraps LiteralExprAST and RangeExprAST for uniform handling.
// The semantic pass recognises ASTKind::LiteralExpr and ASTKind::RangeExpr in
// pattern position and applies match semantics (equality/bounds tests) rather
// than evaluation semantics.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Matches any value and binds it to a name.
 *
 * @example
 *   n            – bind without guard
 *   n if n < 50  – bind with guard (guard lives on MatchArmAST)
 *
 * The bound name is declared by the pattern itself – not looked up in an
 * outer scope. A BindPatternAST without a guard matches everything.
 */
struct BindPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::BindPattern;

    InternedString name;

    explicit BindPatternAST(InternedString n)
        : PatternAST(ASTKind::BindPattern), name(n) {}

};

using BindPatternPtr = BindPatternAST*;

/**
 * @brief Matches any value and discards it – the '_' token.
 *
 * Semantically identical to BindPatternAST except no name is introduced.
 * '_' and 'default' are distinct: '_' is a pattern, 'default' is the fallback arm.
 */
struct WildcardPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::WildcardPattern;

    WildcardPatternAST() : PatternAST(ASTKind::WildcardPattern) {}
};

using WildcardPatternPtr = WildcardPatternAST*;

/**
 * @brief Wraps a LiteralExprAST or RangeExprAST for use in pattern contexts.
 *
 * The semantic pass recognises the wrapped expression and applies match
 * semantics (equality test for literals, bounds test for ranges).
 */
struct PatternExprAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::PatternExpr;

    ExprAST* inner;   // LiteralExprAST or RangeExprAST

    PatternExprAST(ExprAST* expr) : PatternAST(ASTKind::PatternExpr), inner(expr) {}
};

/**
 * @brief Matches when the subject is of a specific type and binds the narrowed value.
 *
 * @example
 *   s is Circle   – matches if subject is Circle, binds as 's' typed Circle
 *
 * Grammar: IDENTIFIER 'is' type
 */
struct TypePatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::TypePattern;

    InternedString bindName;    // name introduced into arm scope
    TypeAST* checkType;         // the type being tested against

    TypePatternAST() : PatternAST(ASTKind::TypePattern) {}
};

using TypePatternPtr = TypePatternAST*;

/**
 * @brief One field entry inside a struct pattern.
 *
 * @example
 *   x           – shorthand: binds field 'x' to name 'x'
 *   x: 0.0      – full form: matches field 'x' against sub‑pattern 0.0
 *   x: Vec2 {…} – nested pattern
 */
struct FieldPatternAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FieldPattern;

    InternedString field;
    PatternAST* subPattern;

    FieldPatternAST() : BaseAST(ASTKind::FieldPattern) {}
};

using FieldPatternPtr = FieldPatternAST*;

/**
 * @brief Matches when the subject is a struct of the named type and its fields
 *        satisfy the given field patterns.
 *
 * @example
 *   Vec2 { x: 0.0, y: 0.0 }   – exact match on both fields
 *   Vec2 { x, y }              – shorthand: binds x and y from subject
 *   Player { health: 0 }       – only health must be 0, other fields ignored
 *
 * Fields not listed are ignored – the match succeeds as long as the listed
 * fields satisfy their patterns.
 */
struct StructPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::StructPattern;

    InternedString typeName;                     // "Vec2", "Player"
    ArenaSpan<FieldPatternPtr> fields;           // field patterns in source order

    StructPatternAST() : PatternAST(ASTKind::StructPattern) {}
};

using StructPatternPtr = StructPatternAST*;

// ─────────────────────────────────────────────────────────────────────────────
// ARM NODES – used inside MatchExprAST
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief One non‑default arm in a match expression.
 *
 * @example
 *   200           => "ok"
 *   200, 201, 202 => "success"
 *   1..10         => "light"
 *   n if n < 0    => "invalid: " + string(n)
 *   s is Circle   => s.radius * s.radius * 3.14159
 *
 * patterns – one or more patterns (any pattern from above). All patterns in
 *            the list share the same guard and body.
 * guard    – optional filter expression (only valid after bind/wildcard pattern)
 * exprs    – one or two result expressions (primary, optional secondary)
 */
struct MatchArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MatchArm;

    ArenaSpan<PatternAST*> patterns;   // at least one
    ExprAST* guard;                    // nullptr if no guard
    ArenaSpan<ExprAST*> exprs;         // 1 or more result expressions

    MatchArmAST() : BaseAST(ASTKind::MatchArm) {}
};

using MatchArmPtr = MatchArmAST*;

/**
 * @brief The required final fallback arm – always present on every match expression.
 *
 * @example
 *   default => "unknown"
 *   default => "unknown", "no detail available"
 *
 * The 'default' arm has no pattern and no guard – it always matches.
 * It must be the last arm in the match.
 */
struct DefaultArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::DefaultArm;

    ArenaSpan<ExprAST*> exprs;   // 1 or more result expressions

    DefaultArmAST() : BaseAST(ASTKind::DefaultArm) {}
};

using DefaultArmPtr = DefaultArmAST*;

/**
 * @brief Pattern matching expression – always produces a value.
 *
 * @example
 *   match status {
 *       200      => "ok"
 *       404      => "not found"
 *       default  => "unknown"
 *   }
 *
 * Grammar rules enforced by the semantic pass:
 *   - default arm is required and must be last
 *   - all arms must produce the same type
 *   - wildcards and unconditional binds must appear before default
 *   - enum exhaustiveness may be checked when subject is an enum type
 */
struct MatchExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::MatchExpr;

    ExprAST* subject;
    ArenaSpan<MatchArmPtr> arms;
    DefaultArmPtr defaultBody;           // required
    SourceLocation defaultLoc;           // location of 'default' keyword

    MatchExprAST() : ExprAST(ASTKind::MatchExpr) {}
};