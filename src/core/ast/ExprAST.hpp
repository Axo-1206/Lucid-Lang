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
///
/// ─── Removed Nodes ────────────────────────────────────────────────────────
/// This header deliberately has NO node for:
///   - `IntrinsicCallExprAST` — the `#name(...)` intrinsic syntax is removed.
///     Intrinsic-like operations (`sizeof<T>()`, `toStr(v)`, `alloc<T>(n)`,
///     `memcpy(...)`) are ordinary calls resolved through the `DEF` table
///     or bound via `FN ... = #builtin(...)`.
///   - `ArenaAccessExprAST` — the `::` special form for `Arena` is removed.
///     `::` now means module access and static struct member access,
///     represented uniformly by `ModuleAccessExprAST`.
///
/// ─── Field Categories ─────────────────────────────────────────────────────
/// This file follows the field-category convention from BaseAST.hpp:
///   - Parser fields: set once by the parser, immutable after construction.
///   - Semantic fields: set by Sema, mutable, may be queried by later passes.

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"
#include "core/memory/InternedString.hpp"

#include <string>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// LiteralKind
// ─────────────────────────────────────────────────────────────────────────────

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
    Unknown     // broken/related syntax or token
};

// ─────────────────────────────────────────────────────────────────────────────
// AssignOp / BinaryOp / UnaryOp
// ─────────────────────────────────────────────────────────────────────────────

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
/// @note Bitwise operators use single symbols: `&`, `|`, `^`, `<<`, `>>`.
///       Logical operators use keywords: `and`, `or`.
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

// ─────────────────────────────────────────────────────────────────────────────
// LiteralExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A scalar literal value – numbers, strings, characters, booleans, nil, err.
/// 
/// @example
///   42         → kind=Int,       value="42"
///   3.14       → kind=Float,     value="3.14"
///   "hello"    → kind=String,    value="hello"
///   """raw"""  → kind=RawString, value="raw"
///   'A'        → kind=Char,      value="A"
///   0xFF       → kind=Hex,       value="0xFF"
///   0b1010     → kind=Binary,    value="0b1010"
///   true       → kind=True,      value="true"
///   false      → kind=False,     value="false"
///   nil        → kind=Nil,       value="nil"
///   err        → kind=Err,       value="err"
/// 
/// The semantic pass converts the raw lexeme to a typed constant value.
struct LiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::LiteralExpr;

    const LiteralKind kind;
    const InternedString value;   // raw lexeme from the token

    LiteralExprAST(LiteralKind k, InternedString v)
        : ExprAST(ASTKind::LiteralExpr), kind(k), value(std::move(v)) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ArrayLiteralExprAST / FieldInitAST / StructLiteralExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An array literal – a bracketed list of expressions.
/// 
/// @example
///   [1, 2, 3]
///   ["hello", "world"]
///   []  – empty array literal
/// 
/// The array kind (dynamic, slice, fixed) is determined by the declared type
/// of the binding the literal initialises — the literal itself is kind-neutral.
/// The semantic pass sets `resolvedType` after inference.
/// 
/// @note A slice literal (`[_]T`) is not directly constructible from a
///       literal; slices come from slicing an existing array.
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
    ExprAST* value = nullptr;

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
/// 
/// Field order in the literal is free. Fields with defaults may be omitted.
/// Fields without defaults must be provided. A struct with only `@[opaque]`
/// fields has no literal form; it must be constructed by an `FN` in the core
/// script (e.g., `map_new<string, int>()`).
struct StructLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::StructLiteralExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString typeName;
    ArenaSpan<TypeAST*>  genericArgs;  // e.g., [int] for Box<int> { ... }
    ArenaSpan<FieldInitAST*> inits;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// @brief The resolved struct declaration (template or specialized).
    StructDeclAST* resolvedDecl = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    StructLiteralExprAST(InternedString n, ArenaSpan<TypeAST*> args, ArenaSpan<FieldInitAST*> in)
        : ExprAST(ASTKind::StructLiteralExpr), typeName(n), genericArgs(args), inits(in) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// IdentifierExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A bare identifier used as an expression.
/// 
/// @example
///   x              – local variable or parameter
///   add            – function name
///   Direction      – enum type name (used before .North in Direction.North)
///   identity<int>  – a generic specialization reference (genericArgs = [int])
/// 
/// The semantic pass resolves the name against the symbol table and sets
/// `resolvedType`. If the name resolves to a struct field and `self` is in
/// scope (inside a block-body field default), it is lowered to a field access
/// through `self`; see the field-access information below.
///
/// ─── `genericArgs`: Family vs. Member ──────────────────────────────────
/// This single node covers both a plain name and a generic specialization
/// reference — there is no separate `generic_ref_expr` AST node. `genericArgs`
/// distinguishes the two cases:
///
///   - `genericArgs.empty()` — a plain name (`x`, `add`, `identity`). If
///     `resolvedDecl` resolves to a generic `FuncDeclAST`
///     (`resolvedDecl->isGeneric()`), this identifier names a *family*,
///     not a value — Sema rejects it wherever a value is required
///     (diagnostic D2: "'identity' names a generic function, not a value").
///     A bare generic name is only legal as the callee of a `CallExprAST`
///     that itself carries generic arguments (e.g., `identity<int>(x)`,
///     where the type arguments live on the callee expression).
///
///   - `!genericArgs.empty()` — a specialization reference (`identity<int>`).
///     This denotes a concrete function value — a *member* of the family,
///     structurally identical to a hand-written concrete function once
///     monomorphized. It is valid in any expression position.
///
///     Whether this node is itself a **call** or a **bare reference** is
///     determined structurally, by the parent node: if this
///     `IdentifierExprAST` is the `callee` of a `CallExprAST`, it's being
///     called; if it stands alone, it's a bare specialization reference
///     used as a value.
///
/// NOTE: `resolvedDecl` already points to the appropriate function
/// declaration (template or specialized).
struct IdentifierExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IdentifierExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString name;
    ArenaSpan<TypeAST*> genericArgs;  // e.g., [int] for identity<int>

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    ValueDeclAST* resolvedDecl = nullptr;  // template OR specialized function

    // ─── Implicit Field Access Through Self ─────────────────────────────
    /// @brief True if this identifier was resolved as a field access through `self`.
    /// Only set inside a block-body function-typed field default.
    bool isImplicitFieldAccess = false;

    /// @brief The `self` parameter expression (for CodeGen to use as the object).
    ExprAST* selfObject = nullptr;

    /// @brief The field index for fast access (set by Sema when
    ///        `isImplicitFieldAccess` is true).
    size_t fieldIndex = SIZE_MAX;

    explicit IdentifierExprAST(InternedString n)
        : ExprAST(ASTKind::IdentifierExpr), name(n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldAccessExprAST — struct field or enum variant access via '.'.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Accesses a data member (struct field or enum variant) via `.`.
/// 
/// @example
///   v.x                     → object = identifier("v"), field = "x"
///   Direction.North         → object = identifier("Direction"), field = "North"
///   JsonValue.Num(3.14)     → the `.Num` part is a FieldAccessExpr; the call
///                             is a CallExpr whose callee is this node
/// 
/// The semantic pass resolves the field or variant and caches the result on
/// this node. For enum variant access, `isEnumAccess` is set and `ownerType`
/// names the enum; a call wrapping this node constructs the payload.
struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* object = nullptr;
    const InternedString fieldName;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// @brief The resolved declaration (FieldDeclAST or EnumVariantAST).
    ValueDeclAST* resolvedDecl = nullptr;

    /// @brief The type that owns this field (StructDeclAST or EnumDeclAST).
    TypeDeclAST* ownerType = nullptr;

    /// @brief True if this is an enum variant access (e.g., Direction.North).
    bool isEnumAccess = false;

    /// @brief The index of this field in the struct, or the variant index
    ///        for enum access.
    size_t fieldIndex = SIZE_MAX;

    FieldAccessExprAST(InternedString n)
        : ExprAST(ASTKind::FieldAccessExpr), fieldName(n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ModuleAccessExprAST — module member or struct static member via '::'.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Accesses a module member or struct static member via `::`.
/// 
/// The `::` operator is used uniformly for module member access and struct
/// static member access. The left-hand side may be a module name or a struct
/// type name; Sema resolves which and sets `isStaticStructAccess`.
/// 
/// @example
///   math::sqrt(x)        → lhsName = "math",  memberName = "sqrt"  (module access)
///   Vec2::zero()         → lhsName = "Vec2",  memberName = "zero"  (static struct access)
///   mymod::PI            → lhsName = "mymod", memberName = "PI"    (module value)
///   Box<int>::default()  → lhsName = "Box",   memberName = "default", genericArgs = [int]
/// 
/// @note A `::` access never resolves to a struct *instance* field. Instance
///       fields are accessed with `.` (see `FieldAccessExprAST`).
struct ModuleAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ModuleAccessExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString lhsName;      // module name OR struct type name
    const InternedString memberName;   // exported member OR static member name
    ArenaSpan<TypeAST*> genericArgs;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    ValueDeclAST* resolvedDecl = nullptr;
    TypeDeclAST*  ownerType = nullptr;          // for static struct access
    bool isStaticStructAccess = false;          // true if LHS resolved to a struct type

    ModuleAccessExprAST(InternedString lhs, InternedString mem)
        : ExprAST(ASTKind::ModuleAccessExpr), lhsName(lhs), memberName(mem) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CallExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A function call – supports regular calls, generic instantiation,
///        and argument pack (`!`) for pipeline injection.
/// 
/// @example
///   f(1, 2, 3)                              → callee is IdentifierExprAST
///   identity<int>(x)                        → callee has genericArgs = [int]
///   math::sqrt(x)                           → callee is ModuleAccessExprAST
///   x |> map<int, string>(stringFromInt)!   → hasArgPack = true
///   JsonValue.Num(3.14)                     → callee is FieldAccessExprAST
/// 
/// ─── Generic Instantiation ──────────────────────────────────────────────
/// Generic arguments are stored on the callee expression
/// (`IdentifierExprAST::genericArgs` or `ModuleAccessExprAST::genericArgs`).
/// The callee remains a plain function reference; the generic arguments are
/// applied at the call site.
/// 
/// ─── Argument Pack (`!`) ────────────────────────────────────────────────
/// `fn(args)!` is not a function call in the ordinary sense — the `!` marks
/// an intentionally incomplete argument list. When the enclosing pipeline
/// step fires, the upstream value is injected as the **first** argument.
/// Sema verifies that `hasArgPack` is only true when the call is inside a
/// pipeline step.
/// 
/// ─── Return Type ────────────────────────────────────────────────────────
/// A call produces exactly one value. Functions that need to return several
/// values return a struct; the caller destructures it with a `let` and field
/// access, not with a multi-value binding.
struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* callee = nullptr;          // IdentifierExprAST / ModuleAccessExprAST / FieldAccessExprAST
    ArenaSpan<ExprAST*> args;
    const bool hasArgPack = false;      // true for `fn(args)!`

    // ─── Constructor ─────────────────────────────────────────────────────
    CallExprAST(bool a)
        : ExprAST(ASTKind::CallExpr), hasArgPack(a) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// IndexExprAST / SliceExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Array, slice, or map element access.
/// 
/// @example
///   nums[2]       → array index
///   m["alice"]    → map lookup
/// 
/// The index is runtime-checked for arrays and slices. Out-of-bounds access
/// panics unless guarded with `??`: `nums[i] ?? 0`.
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Runtime check**: Indexing a slice (`[_]T`) or dynamic array (`[*]T`)
///    is always runtime-checked. A literal index does not prove in-bounds
///    against a slice of unknown length.
/// 2. **Compile-time check**: Indexing a fixed-size array (`[N]T`) with a
///    literal index that is provably less than `N` is checked at compile time.
/// 3. **Panic handling**: Out-of-bounds access panics unless guarded with `??`.
/// 4. **Resolution**: The operation is resolved through `DEF INDEX_GET` or
///    `DEF INDEX_SET`. If no matching `DEF` exists for the container type
///    and index type, the expression is a compile error.
/// 
/// @field target   The container being indexed.
/// @field index    The index (integer for arrays/slices; key type for maps).
struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* target = nullptr;
    ExprAST* index = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    IndexExprAST(ExprAST* t, ExprAST* i)
        : ExprAST(ASTKind::IndexExpr), target(t), index(i) {}
};

/// @brief Slice expression – produces a view over a contiguous range.
/// 
/// @example
///   nums[1..3]   → start = 1, end = 3,   isExclusive = false
///   nums[1..<3]  → start = 1, end = 3,   isExclusive = true  (end excluded)
///   nums[..<2]   → start = nullptr, end = 2, isExclusive = true
///   nums[3..]    → start = 3, end = nullptr, isExclusive = false
///   nums[..]     → start = nullptr, end = nullptr, isExclusive = false
/// 
/// ─── Slice Rules ────────────────────────────────────────────────────────
/// 1. **View**: A slice `[_]T` is a view over a contiguous range of another
///    array. It does not own the underlying memory; the backing array must
///    outlive the slice, which the compiler enforces.
/// 2. **Bounds**: Start defaults to 0, end defaults to the array's length.
/// 3. **Runtime check**: Slice bounds are runtime-checked. Out-of-bounds
///    access panics unless guarded with `??`.
/// 4. **Inclusive / exclusive**: `..` is inclusive, `..<` is exclusive.
/// 
/// @field target       The array being sliced.
/// @field start        Inclusive start (nullptr means 0).
/// @field end          End bound (nullptr means array length).
/// @field isExclusive  True for `..<` (end excluded).
struct SliceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::SliceExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* target = nullptr;
    ExprAST* start = nullptr;      // inclusive; nullptr means 0
    ExprAST* end = nullptr;        // inclusive or exclusive depending on isExclusive
    const bool isExclusive;

    // ─── Constructor ─────────────────────────────────────────────────────
    SliceExprAST(ExprAST* t, ExprAST* s, ExprAST* e, bool ex = false)
        : ExprAST(ASTKind::SliceExpr), target(t), start(s), end(e), isExclusive(ex) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// BinaryExprAST / UnaryExprAST / AssignExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An infix binary operation.
/// 
/// @example
///   a + b    → op = Add
///   x == y   → op = Eq
///   p and q  → op = And (short‑circuit, logical)
///   a & b    → op = BitAnd (bitwise AND, integer types only)
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Logical operators**: `and` and `or` are short-circuiting. Their
///    operands are evaluated by the truthiness table:
///      - Concrete non-nullable, non-fallible: always true (compile-time fold).
///      - `bool`: the runtime value.
///      - `T?`: false if `nil` (runtime check).
///      - `T!`: false if `err` (runtime check).
///      - `T?!`: false if `nil` or `err` (runtime check).
///    The result is always `bool`. Operands whose truth value is a
///    compile-time constant are folded at compile time.
/// 2. **Bitwise operators**: `&`, `|`, `^`, `<<`, `>>` are integer-only.
/// 3. **Comparison**: `==` and `!=` compare values. `&` is bitwise AND, not
///    reference-of; there is no reference-equality operator.
/// 4. **Arithmetic**: `+`, `-`, `*`, `/`, `%`, `**` are numeric-only.
/// 5. **Resolution**: Every binary operator is resolved through the
///    `BINARY_OP` `DEF` table, keyed by operand types.
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
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Arithmetic negation**: `-` is numeric-only.
/// 2. **Logical NOT**: `not` accepts any type; evaluated by the truthiness
///    table (see `BinaryExprAST`). The result is always `bool`.
/// 3. **Bitwise NOT**: `~` is integer-only.
/// 4. **No reference-of**: There is no unary `&x` operator in expression
///    position. `&T` in type position is a reference type marker handled by
///    `RefTypeAST`, not by `UnaryExprAST`. The `&` symbol in expression
///    position is bitwise AND (a binary operator).
/// 5. **Resolution**: `-` and `~` resolve through `UNARY_OP`; `not` has
///    fixed truthiness semantics and resolves to a `bool` result directly.
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
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Lvalue required**: The left-hand side must be an assignable lvalue
///    (a `let` binding, a mutable field access, or a mutable index).
/// 2. **Const checking**: Assigning to a `const` variable or `const` field
///    is a semantic error.
/// 3. **Type matching**: The right-hand side type must be assignable to the
///    left-hand side type.
struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    const AssignOp op;
    ExprAST* lhs = nullptr;
    ExprAST* rhs = nullptr;

    AssignExprAST(AssignOp o)
        : ExprAST(ASTKind::AssignExpr), op(o) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NullCoalesceExprAST — the `??` fallback.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The null coalescing operator – provides a fallback when the LHS
///        is `nil`, `err`, or both.
/// 
/// @example
///   value ?? fallback
///   riskyOp() ?? -1
///   lookup() ?? User { id = 0, name = "guest" }
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Sentinel coverage**: `??` triggers when the left-hand side is `nil`,
///    `err`, or both (for `T?!` types).
/// 2. **Result type**: The result type is the plain type `T` (the unwrapped
///    type of the LHS). The right-hand side must produce a value of that
///    type. `??` always fully resolves the sentinel — the whole expression
///    is never `T?`, `T!`, or `T?!`.
/// 3. **Narrowing**: `??` does not narrow the left-hand side; the LHS
///    binding retains its original nullable/fallible type afterward.
/// 
/// @field value      The nullable/fallible value.
/// @field fallback   The fallback expression.
struct NullCoalesceExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullCoalesceExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* value = nullptr;
    ExprAST* fallback = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    NullCoalesceExprAST(ExprAST* v, ExprAST* f)
        : ExprAST(ASTKind::NullCoalesceExpr), value(v), fallback(f) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// PipelineStepAST / PipelineExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One step in a pipeline chain – owned by `PipelineExprAST`.
///
/// A step is a function-valued expression, optionally with an argument pack.
/// 
/// ─── Two Forms ──────────────────────────────────────────────────────────
///   - `expr` — a single-argument function value. The upstream value is
///     passed as its only argument. `packArgs` is empty.
///   - `expr(args)!` — a call with an argument pack. The upstream value is
///     injected as the first argument; the remaining arguments fill the rest.
///     `packArgs` is non-empty.
///
/// The `!` is mandatory for the second form; `f(args)` without `!` is not a
/// valid pipeline step.
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Function-valued**: The `callable` must resolve to a value of a
///    function type. Generic functions must be instantiated with explicit
///    type arguments at the step site.
/// 2. **Argument injection**: When the step fires, the upstream value is
///    injected into the first unfilled parameter of the callee.
///
/// @field callable   The function-valued expression.
/// @field packArgs   The step's explicit arguments (empty for the bare form).
struct PipelineStepAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineStep;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* callable = nullptr;
    ArenaSpan<ExprAST*> packArgs;

    // ─── Constructor ─────────────────────────────────────────────────────
    PipelineStepAST(ExprAST* c, ArenaSpan<ExprAST*> p)
        : BaseAST(ASTKind::PipelineStep), callable(c), packArgs(p) {}
};

/// @brief A runtime pipeline chain – `seed |> step |> step |> ...`
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Left-to-right**: The pipeline executes left to right at runtime.
/// 2. **Argument injection**: Each step's upstream value is injected as the
///    first argument (or the only argument, for the bare form).
/// 3. **Step type checking**: Each step's parameter type must accept the
///    upstream value; the step's return type becomes the next step's
///    upstream type.
/// 4. **Short-circuit**: The pipeline does not short-circuit on `nil`/`err`
///    by itself; a fallible intermediate must be handled with `??` or
///    narrowed before the next step.
/// 
/// @field seed    The initial value.
/// @field steps   Pipeline steps in order (at least one).
struct PipelineExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* seed = nullptr;
    ArenaSpan<PipelineStepAST*> steps;

    // ─── Constructor ─────────────────────────────────────────────────────
    PipelineExprAST(ExprAST* s, ArenaSpan<PipelineStepAST*> st)
        : ExprAST(ASTKind::PipelineExpr), seed(s), steps(st) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// AnonFuncExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An anonymous function expression — the only node that holds a
///        function body.
///
/// ─── Design: The Sole Body-Holder ──────────────────────────────────────
/// A function body exists in exactly one place in the AST: here. A named
/// function declaration (`FuncDeclAST`) with a block body is parsed into
/// an `AnonFuncExprAST` and stored as the declaration's initializer. A
/// field with a block-body default is parsed the same way. This node is
/// what CodeGen's closure-lowering machinery consumes directly, with no
/// adapter and no synthesized "view" node.
///
/// ─── Type Comes From the Node Itself ───────────────────────────────────
/// This node is self-typed: `funcType` is the signature. When a body is
/// borrowed from a declaration header (a block-body function declaration),
/// the parser sets `funcType` from that header. When a body is written
/// inline (a `func_literal`), the node carries its own signature.
///
/// Sema checks compatibility (`funcType` vs. the declaration site's
/// declared type) as an ordinary assignment, not an inference step.
///
/// ─── The Runtime Parameters Live Here ──────────────────────────────────
/// For a block-body `FuncDeclAST` (`const add (a int) -> int = { ... }`),
/// the AST contains two `FuncTypeAST` nodes with `ParamAST` children:
///
///   - `FuncDeclAST::funcType` — the *declared* signature, parsed from
///     the declaration header. Its `ParamAST` nodes are type-only: never
///     allocated, never bound, used only for signature comparison.
///
///   - `AnonFuncExprAST::funcType` (this node's field) — the *runtime*
///     signature, parsed from the block body. Its `ParamAST` nodes are
///     the real parameters. CodeGen iterates *this* field to allocate
///     each parameter's stack slot and register it as a binding. Body
///     identifiers resolve against these nodes.
///
/// Reassignment reinforces the point: `f = (n int) -> int { ... };`
/// replaces the declaration's `init` with a *new* `AnonFuncExprAST` that
/// carries its *own* `ParamAST` for `n`. The declared `FuncDeclAST::funcType`
/// is unchanged and still lists the original parameter; the runtime
/// parameter is the one on the new `AnonFuncExprAST::funcType`. CodeGen
/// reads parameters from this field on the *current* init, every time it
/// lowers a body.
///
/// ─── No `genericParams` Here, By Design ────────────────────────────────
/// This node has no `genericParams` field, and none was added when
/// `FuncDeclAST` gained its const-only-for-generics invariant. That
/// invariant falls out of this node's shape for free: an anonymous
/// function literal has no syntax to declare type parameters of its own
/// (`func_literal` in the grammar carries no `generic_params`), so an
/// `AnonFuncExprAST` can never itself be generic — only the enclosing
/// `FuncDeclAST` can be.
struct AnonFuncExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AnonFuncExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    FuncTypeAST* funcType = nullptr;
    StmtAST* body = nullptr;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// Variables captured by this closure, if any. Empty for a
    /// non-capturing function.
    ///
    /// Invariant: `hasClosure == (captures.size() > 0)`.
    ArenaSpan<CapturedVariable> captures;
    bool hasClosure = false;

    bool hasParams() const { return funcType && !funcType->params.empty(); }

    AnonFuncExprAST(FuncTypeAST* ft, StmtAST* b)
        : ExprAST(ASTKind::AnonFuncExpr), funcType(ft), body(b) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// IfExprAST — the expression form of `if`.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The expression form of `if` – both branches required, both produce the same type.
/// 
/// @example
///   if score >= 60 ?? "pass" else "fail"
///   if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
/// 
/// Grammar: `if_expr := 'if' expr '??' expr 'else' expr`
/// 
/// This is distinct from `IfStmtAST` (in StmtAST.hpp), where `else` is
/// optional and no value is produced. The parser distinguishes the two by
/// the presence of `??` after the condition.
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Else required**: Both branches must be present.
/// 2. **Type matching**: Both branches must produce compatible types; the
///    expression's result type is the common type of the two branches.
/// 3. **Chaining**: Chained if-expressions are right-associative.
/// 
/// @field condition    The condition expression (evaluated by truthiness).
/// @field thenBranch   The then branch (expression).
/// @field elseBranch   The else branch (expression).
struct IfExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IfExpr;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* condition = nullptr;
    ExprAST* thenBranch = nullptr;
    ExprAST* elseBranch = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    IfExprAST(ExprAST* cond, ExprAST* then_, ExprAST* else_)
        : ExprAST(ASTKind::IfExpr),
          condition(cond), thenBranch(then_), elseBranch(else_) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// RangeExprAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A range – `lo..hi` or `lo..<hi`.
/// 
/// @example
///   0..10   – inclusive: 0, 1, 2, ..., 10
///   0..<10  – exclusive: 0, 1, 2, ..., 9
/// 
/// ─── Where Ranges Appear ────────────────────────────────────────────────
/// A range is not a standalone value. It appears in exactly three positions:
///   - Range iteration in `for` (`for i int in 0..10`).
///   - Slice bounds (`nums[1..3]`).
///   - `switch` case values (`case 1..10:`).
/// 
/// A range has no type of its own and cannot be stored in a variable or
/// passed to a function.
/// 
/// ─── Bounds ─────────────────────────────────────────────────────────────
/// In `for` and slice positions, bounds may be arbitrary expressions of a
/// common numeric type. In `switch` cases, both bounds must be compile-time
/// literals.
/// 
/// @field lo           Start (inclusive).
/// @field hi           End (inclusive for `..`, exclusive for `..<`).
/// @field isExclusive  True for `..<`.
struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprAST* lo = nullptr;   // start (inclusive)
    ExprAST* hi = nullptr;   // end (inclusive or exclusive)
    const bool isExclusive = false;

    RangeExprAST(bool ex)
        : ExprAST(ASTKind::RangeExpr), isExclusive(ex) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CaseValueAST — one match value inside a `switch` case.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One match value inside a `switch` case clause, with an optional
///        payload binding for payload-carrying enum variants.
///
/// A `case` clause can match multiple values; each value is represented by a
/// `CaseValueAST`. When the matched value is a payload-carrying enum variant,
/// `hasBinding` is true and `binding` names the variable introduced into the
/// case body's scope.
///
/// @example
///   case 200:                     → value = LiteralExpr(200), hasBinding = false
///   case Direction.North:         → value = FieldAccessExpr, hasBinding = false
///   case 1..10:                   → value = RangeExprAST, hasBinding = false
///   case JsonValue.Num(n):        → value = FieldAccessExpr (on JsonValue.Num),
///                                    binding = "n", hasBinding = true
///
/// The `value` expression is the match target. For a payload variant, Sema
/// recognizes that the value is a `FieldAccessExpr` resolving to an
/// `EnumVariantAST` with a payload type; the binding is the name of the
/// variable to introduce.
///
/// @field value       The match value — a literal, enum variant, or range expression.
/// @field binding     The payload binding name (non-empty iff hasBinding).
/// @field hasBinding  True if this case value introduces a payload variable.
struct CaseValueAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::CaseValue;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST*       value = nullptr;   // literal, enum variant, or range
    InternedString binding;           // payload binding name, or empty
    const bool     hasBinding;        // true iff a payload variable is introduced

    explicit CaseValueAST(bool hb = false)
        : BaseAST(ASTKind::CaseValue), hasBinding(hb) {}
};