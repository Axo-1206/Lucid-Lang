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
// PipelineStepKind – syntactic form of a pipeline step.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Distinguishes the different syntactic forms a pipeline step can take.
 *
 * Stored on PipelineStepAST so the semantic pass can apply the correct rules
 * without re‑parsing the node structure.
 */
enum class PipelineStepKind {
    Ident,            // fn – bare function name
    BehaviorRef,      // obj:method – impl method reference
    FieldRef,         // obj.field – data field of non‑nullable function type
    IndexRef,         // arr[idx] – function reference

    ArgPack,          // fn(args)! – upstream injected as first argument
    BehaviorArgPack,  // obj:method(args)! – method with argument pack
    FieldArgPack,     // obj.field(args)! – field with argument pack
    IndexArgPack,     // arr[idx](args)! – array instance with argument pack

    AnonFunc,         // (x T) R { } – inline anonymous function, (single param group)
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
    ExprPtr value;       // initialiser expression

    FieldInitAST() : BaseAST(ASTKind::FieldInit) {}
    FieldInitAST(InternedString n, ExprPtr v)
        : BaseAST(ASTKind::FieldInit), name(n), value(std::move(v)) {}

};

using FieldInitPtr = ASTPtr<FieldInitAST>;

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
    ArenaSpan<TypePtr> genericArgs;        // empty if non‑generic
    ArenaSpan<FieldInitPtr> inits;         // field = expr entries
    ASTPtr<NamedTypeAST> instantiatedType; // semantic cache

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

    explicit IdentifierExprAST(InternedString n)
        : ExprAST(ASTKind::IdentifierExpr), name(n) {}
};

/**
 * @brief Accesses a data member (struct field) via the '.' operator.
 *
 * @example
 *   v.x             → object = identifier("v"), field = "x"
 *   player.health   → object = identifier("player"), field = "health"
 *   Direction.North → object = identifier("Direction"), field = "North" (enum variant)
 *
 * The '.' operator always means data – a field declared inside a struct body,
 * or an enum variant. Impl methods use ':' (BehaviorAccessExprAST).
 */
struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    ExprPtr object;
    InternedString field;

    FieldAccessExprAST() : ExprAST(ASTKind::FieldAccessExpr) {}
};

/**
 * @brief Accesses a method on a value via the ':' operator.
 *
 * @example
 *   v:normalize() → typeName = type of v, method = "normalize"
 *   v:length()    → typeName = type of v, method = "length"
 *
 * The left‑hand side of ':' is an expression (a value), not a type name.
 * The semantic pass resolves the receiver's type, then looks up the method
 * in the impl blocks for that type.
 *
 * Behavior members are never reassignable – the semantic pass enforces this
 * via the isBehaviorMember flag.
 *
 * The result is a plain function reference – can be stored, passed as an
 * argument, or used as a pipeline step.
 *
 * ## Codegen Annotations (written by semantic Phase 3b)
 *
 * - concreteTypeArgs: concrete type args from the receiver's declared type
 * - resolvedMangledName: fully qualified LLVM function name for direct lookup
 */
struct BehaviorAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BehaviorAccessExpr;

    InternedString typeName; // resolved type name (for mangling)
    InternedString method;   // method name

    // Codegen annotations
    ArenaSpan<InternedString> concreteTypeArgs; // from receiver's type
    std::string resolvedMangledName;            // for direct registry lookup
    ArenaSpan<TypePtr> genericArgs;             // explicit generic args

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
 *   f(1, 2, 3)                              → isArgPack = false, genericArgs empty
 *   Buffer<int>(capacity)                   → genericArgs = [Int]
 *   obj:method(a, b)                        → callee is BehaviorAccessExprAST
 *   arr[idx](x, y)                          → callee is IndexExprAST
 *   pipelineFn(2.0)!                        → isArgPack = true (upstream injected as first arg)
 *   handlers[i](event)                      → callee is IndexExprAST
 *
 * @par Generic Instantiation
 *   Generic arguments are stored in `genericArgs`. The callee remains a plain
 *   function reference; the generic arguments are applied at the call site.
 *
 * @par Argument Pack (`!`)
 *   When `isArgPack` is true, the argument list is intentionally incomplete.
 *   The upstream value from a `|>` pipeline will be injected as the first argument
 *   during semantic transformation. This is only valid inside a pipeline step.
 *
 * @par Async Calls
 *   `isAsyncCall` is set by the semantic pass after resolving the callee.
 *   If the callee's type has the `~async` qualifier, this flag is true and
 *   the call must be preceded by `await` (unless inside another async function).
 *
 */
struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    ExprPtr callee;                 // The callable expression
    ArenaSpan<TypePtr> genericArgs; // Explicit type arguments (e.g., <int>)
    ArenaSpan<ExprPtr> args;        // Call arguments in order
    bool isArgPack = false;         // true → fn(args)! — pipeline argument pack
    bool isAsyncCall = false;       // true if calling an async function

    CallExprAST() : ExprAST(ASTKind::CallExpr) {}
};

/**
 * @brief Array element access or slice expression.
 *
 * @example
 *   nums[2]      → kind = Element, index = 2, sliceEnd = nullptr
 *   nums[1..3]   → kind = Slice,   index = 1, sliceEnd = 3
 *
 * For Element: index holds the element index, sliceEnd is null.
 * For Slice:   index holds the start, sliceEnd holds the end (both inclusive).
 */
struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    ExprPtr target;
    ExprPtr index;     // element index or slice start
    ExprPtr sliceEnd;  // slice end – nullptr for Element kind
    IndexKind kind;
    bool isExclusive = false; // true if ..< syntax used

    // Semantic cache: owned SliceTypeAST when kind == Slice
    mutable ASTPtr<TypeAST> sliceType;

    IndexExprAST() : ExprAST(ASTKind::IndexExpr) {}
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
 *   ~~x     → op = BitNot
 *   &x      → op = Ref (take a reference)
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
 */
struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    AssignOp op;
    ExprPtr lhs;
    ExprPtr rhs;

    AssignExprAST() : ExprAST(ASTKind::AssignExpr) {}
};

/**
 * @brief Runtime type check – produces bool and narrows type in enclosing block.
 *
 * @example
 *   x is int
 *   shape is Circle
 *   stage is ShaderStage.Fragment
 *
 * After a successful `if x is SomeType { ... }`, x is treated as SomeType
 * inside the block.
 */
struct IsExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IsExpr;

    ExprPtr expr;
    TypePtr checkType;   // the type being checked against

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

    ExprPtr object;
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
 * Not an ExprAST node because it cannot appear independently as an expression.
 * The kind field determines which other fields are populated.
 */
struct PipelineStepAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineStep;

    PipelineStepKind kind;
    InternedString ident;                    // function name, object name
    ArenaSpan<TypePtr> genericArgs;          // explicit generic arguments
    InternedString typeName;                 // for BehaviorRef / BehaviorArgPack
    InternedString method;                   // for BehaviorRef / BehaviorArgPack
    InternedString field;                    // for FieldRef / FieldArgPack
    ExprPtr index;                           // for IndexRef / IndexArgPack
    ArenaSpan<ExprPtr> packArgs;             // for ArgPack variants
    ExprPtr anonFunc;                        // for AnonFunc

    PipelineStepAST() : BaseAST(ASTKind::PipelineStep) {}
};

using PipelineStepPtr = ASTPtr<PipelineStepAST>;

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

    ExprPtr seed;
    ArenaSpan<PipelineStepPtr> steps;   // at least one

    PipelineExprAST() : ExprAST(ASTKind::PipelineExpr) {}
};

/**
 * @brief One operand in a +> composition chain – owned by ComposeExprAST.
 *
 * Three syntactic forms – no ArgPack, no AnonFunc.
 */
struct ComposeOperandAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeOperand;

    ComposeOperandKind kind;
    InternedString ident;       // Ident / FieldRef
    InternedString typeName;    // BehaviorRef – "Vec2"
    InternedString method;      // BehaviorRef – "normalize"
    InternedString field;       // FieldRef

    ComposeOperandAST() : BaseAST(ASTKind::ComposeOperand) {}
};

using ComposeOperandPtr = ASTPtr<ComposeOperandAST>;

/**
 * @brief A compile‑time function composition chain – f +> g +> h
 *
 * @example
 *   let process = validate +> transform +> render
 *
 * +> enforces strict type matching at compile time: the output type of the
 * left operand must exactly match the input type of the next operand.
 */
struct ComposeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeExpr;

    ExprPtr left;
    ArenaSpan<ComposeOperandPtr> operands;   // right‑hand operands in order

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
 * The signature (parameters, return types) is stored in `sig`.
 * The body is always a BlockStmtAST.
 */
struct AnonFuncExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AnonFuncExpr;

    FuncSignature sig;
    StmtPtr body;   // always BlockStmtAST

    bool hasParams() const { return sig.hasParams(); }

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

    ExprPtr inner;   // the async expression being awaited

    explicit AwaitExprAST(ExprPtr e)
        : ExprAST(ASTKind::AwaitExpr), inner(std::move(e)) {}

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
    TypePtr        bindType;   ///< Plain T (never T!E — ! is consumed by resolve)
    StmtPtr        body;       ///< Always BlockStmtAST

    OkArmAST() : BaseAST(ASTKind::OkArm) {}
};

using OkArmPtr = ASTPtr<OkArmAST>;

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
    TypePtr        bindType;   ///< Error type E; nullptr when bare '!' (no error value)
    StmtPtr        body;       ///< Always BlockStmtAST

    /// True when the enclosing result type was bare '!' (no error payload)
    bool isBareError() const { return bindType == nullptr; }

    ErrArmAST() : BaseAST(ASTKind::ErrArm) {}
};

using ErrArmPtr = ASTPtr<ErrArmAST>;

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

    ExprPtr    subject; // The T!E expression being resolved
    OkArmPtr   okArm;   // Required ok arm
    ErrArmPtr  errArm;  // Required err arm

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
 *   0..10   – used in for loops: for i in 0..10
 *   1..10   – used in match range patterns: case 1..10
 *   1..3    – used in slice index: nums[1..3]
 *
 * Both ends are inclusive for '..' – 0..10 iterates 0,1,2,...,10 (11 steps).
 * When isExclusive is true, the end is exclusive (..< syntax).
 */
struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprPtr lo;   // start (inclusive)
    ExprPtr hi;   // end (inclusive/exclusive depends on flag)
    bool isExclusive = false;   // true for ..<

    RangeExprAST() : ExprAST(ASTKind::RangeExpr) {}
};

/**
 * @brief An explicit type cast – safe primitive cast or unsafe bit reinterpret.
 *
 * @example
 *   float(x)       – safe cast
 *   *float(bits)   – unsafe bit reinterpret (isUnsafe = true)
 *
 * Safe casts are only allowed for widening primitive conversions and enum→int.
 * Unsafe casts are only allowed inside @extern declarations.
 */
struct TypeConvExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::TypeConvExpr;

    TypePtr targetType; // the type to convert to
    ExprPtr expr;       // the value being converted
    bool isUnsafe;      // true → *T(expr) bit reinterpret

    TypeConvExprAST(TypePtr t, ExprPtr e, bool unsafe = false)
        : ExprAST(ASTKind::TypeConvExpr),
          targetType(std::move(t)), expr(std::move(e)), isUnsafe(unsafe) {}

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
    TypePtr typeArg;              // for #sizeof(T) – nullptr otherwise
    ArenaSpan<ExprPtr> args;      // value arguments in order

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

/**
 * @brief Wraps a LiteralExprAST or RangeExprAST for use in pattern contexts.
 *
 * The semantic pass recognises the wrapped expression and applies match
 * semantics (equality test for literals, bounds test for ranges).
 */
struct PatternExprAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::PatternExpr;

    ExprPtr inner;   // LiteralExprAST or RangeExprAST

    PatternExprAST(ExprPtr expr) : PatternAST(ASTKind::PatternExpr), inner(std::move(expr)) {}
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
    TypePtr checkType;          // the type being tested against

    TypePatternAST() : PatternAST(ASTKind::TypePattern) {}
};

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
    ASTPtr<PatternAST> subPattern;

    FieldPatternAST() : BaseAST(ASTKind::FieldPattern) {}
};

using FieldPatternPtr = ASTPtr<FieldPatternAST>;

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

    ArenaSpan<ASTPtr<PatternAST>> patterns;   // at least one
    ExprPtr guard;                            // nullptr if no guard
    ArenaSpan<ExprPtr> exprs;                 // 1 or more result expressions

    MatchArmAST() : BaseAST(ASTKind::MatchArm) {}
};

using MatchArmPtr = ASTPtr<MatchArmAST>;

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

    ArenaSpan<ExprPtr> exprs;   // 1 or more result expressions

    DefaultArmAST() : BaseAST(ASTKind::DefaultArm) {}
};

using DefaultArmPtr = ASTPtr<DefaultArmAST>;

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

    ExprPtr subject;
    ArenaSpan<MatchArmPtr> arms;
    DefaultArmPtr defaultBody;           // required
    SourceLocation defaultLoc;           // location of 'default' keyword

    MatchExprAST() : ExprAST(ASTKind::MatchExpr) {}
};