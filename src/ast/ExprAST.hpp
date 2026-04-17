/**
 * @file ExprAST.hpp
 *
 * @responsibility Defines all nodes that evaluate to a value (Calculations, Calls, Literals).
 *
 * @hierarchy BaseAST -> ExprAST -> [Concrete Nodes]
 *
 * @related_files
 *   - src/parser/ParserExpr.cpp (The primary producer)
 *   - src/ast/StmtAST.hpp (Statements often contain expressions)
 *
 * @note
 *   - PatternAST is removed then merge into ExprAST(to reduce cyclic dependency).
 *   - LiteralPatternAST and RangePatternAST are removed.
 *   - LiteralExprAST and RangeExprAST are used directly in pattern position.
 *   - MatchArmAST::patterns now holds BaseAST* instead of PatternAST*.
 *   - StructPatternAST::subPattern now holds BaseAST* instead of PatternAST*.
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"    // ParamAST, ParamPtr — AnonFuncExprAST needs params

#include <string>
#include <vector>
#include <memory>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// ExprAST.hpp — all expression nodes, match arm nodes, and pattern nodes
//
// Every expression node here inherits from ExprAST (defined in BaseAST.hpp).
// StmtAST.hpp includes this header — statements contain expressions.
//
// Pattern nodes (BindPatternAST, WildcardPatternAST, TypePatternAST,
// StructPatternAST) also live here instead of a separate PatternAST.hpp.
// This avoids a circular include: PatternAST.hpp would need ExprAST.hpp for
// guard expressions and arm bodies, and ExprAST.hpp would need PatternAST.hpp
// for MatchExprAST — a cycle. Keeping everything in one file breaks it cleanly.
//
// Node inventory:
//
//   Literals
//     LiteralExprAST          — 42, 3.14, "hello", r"raw", 'a', 0xFF, 0b1010, true, false, nil
//     ArrayLiteralExprAST     — [1, 2, 3]  (kind inferred from declared type)
//     StructLiteralExprAST    — Vec2 { x = 1.0  y = 2.0 }
//
//   Names & access
//     IdentifierExprAST       — bare name: x, foo, Direction
//     FieldAccessExprAST      — v.x  (data member, . operator)
//     BehaviorAccessExprAST   — Vec2:normalize  (impl method, : operator)
//
//   Calls & indexing
//     CallExprAST             — f(args)  or  T<U>(args)
//     IndexExprAST            — nums[i]  or  nums[i..j]  (IndexKind distinguishes)
//
//   Operators
//     BinaryExprAST           — a + b, a == b, a and b, a | b  (all infix binary ops)
//     UnaryExprAST            — -x, not x, ~x, &x
//     AssignExprAST           — x = expr, x += expr, x -= expr, ...
//     IsExprAST               — x is int, shape is Circle  (type check + narrowing)
//
//   Nullable chain
//     NullableChainExprAST    — player.?weapon.?damage ?? 0
//
//   Pipeline & composition
//     PipelineExprAST         — seed -> step -> step
//     PipelineStepAST         — one step in a pipeline (not an ExprAST — see below)
//     ComposeExprAST          — f +> g +> h
//     ComposeOperandAST       — one operand in a compose chain (not an ExprAST)
//
//   Functions
//     AnonFuncExprAST         — (x int) int { ... }  or  async (x int) int { ... }
//     AwaitExprAST            — await httpGet(url)
//
//   Control flow expressions
//     MatchExprAST            — match expr { arm* default }
//                               each arm body is one or two comma-separated exprs
//     IfExprAST               — if cond ?? thenExpr else elseExpr  (inline form)
//
//   Other
//     RangeExprAST            — 0..10 / 0..<10  (for loops, match patterns, slice indexing)
//     TypeConvExprAST         — float(x)  safe explicit cast  |  *float(x)  unsafe bit reinterpret
//
//   Match infrastructure  (not ExprAST — BaseAST directly)
//     MatchArmAST             — pattern_list [guard] -> expr [, expr]
//     DefaultArmAST           — default -> expr [, expr]
//
//   Pattern nodes  (PatternAST — matched against the subject, never evaluated)
//     BindPatternAST          — n  (captures matched value into name)
//     WildcardPatternAST      — _  (matches anything, discards value)
//     TypePatternAST          — v is Circle  (bind + narrow to concrete type)
//     StructPatternAST        — Vec2 { x: 0.0, y }  (struct field destructuring)
//
//   Retired pattern nodes (removed — reuse existing ExprAST nodes directly):
//     LiteralPatternAST       — replaced by LiteralExprAST in pattern position
//     RangePatternAST         — replaced by RangeExprAST in pattern position
//
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// LiteralKind — which literal token was written.
// The parser maps the token type to this enum before constructing the node.
// ─────────────────────────────────────────────────────────────────────────────

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
// AssignOp — the assignment operator written in source.
// Compound operators desugar to x = x op expr at semantic time.
// ─────────────────────────────────────────────────────────────────────────────

enum class AssignOp {
    Assign,    // =
    AddAssign, // +=
    SubAssign, // -=
    MulAssign, // *=
    DivAssign, // /=
    PowAssign, // ^=
    ModAssign, // %=
};

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOp — every infix binary operator in Luc.
// Parser maps token(s) → BinaryOp before constructing BinaryExprAST.
// ─────────────────────────────────────────────────────────────────────────────

enum class BinaryOp {
    // Arithmetic
    Add,  // +
    Sub,  // -
    Mul,  // *
    Div,  // /
    Pow,  // ^
    Mod,  // %

    // Comparison
    Eq,   // ==
    Ne,   // !=
    Lt,   // <
    Gt,   // >
    Le,   // <=
    Ge,   // >=

    // Logical
    And,  // and
    Or,   // or

    // Bitwise
    BitAnd,  // &
    BitOr,   // |
    BitXor,  // ~^
    Shl,     // <<
    Shr,     // >>
};

// ─────────────────────────────────────────────────────────────────────────────
// UnaryOp — every prefix unary operator in Luc.
// ─────────────────────────────────────────────────────────────────────────────

enum class UnaryOp {
    Neg,    // -x       arithmetic negation
    Not,    // not x    logical negation
    BitNot, // ~x       bitwise NOT
    Ref,    // &x       take a reference
};

// ─────────────────────────────────────────────────────────────────────────────
// PipelineStepKind — which syntactic form a pipeline step was written in.
// Stored on PipelineStepAST so the semantic pass can apply the correct rules
// for each form without re-parsing the node structure.
// ─────────────────────────────────────────────────────────────────────────────

enum class PipelineStepKind {
    Ident,        // fn          — bare function name
    BehaviorRef,  // Type:method — impl method reference
    FieldRef,     // obj.field   — data field of non-nullable function type
    ArgPack,      // fn(args)!   — argument pack, upstream injected as first arg
    AnonFunc,     // (x T) R { } — inline anonymous function
};

// ─────────────────────────────────────────────────────────────────────────────
// ComposeOperandKind — which syntactic form a compose operand was written in.
// Mirrors PipelineStepKind but without ArgPack (! is only valid in pipelines).
// ─────────────────────────────────────────────────────────────────────────────

enum class ComposeOperandKind {
    Ident,        // fn
    BehaviorRef,  // Type:method
    FieldRef,     // obj.field  — non-nullable only
};


// ═════════════════════════════════════════════════════════════════════════════
// LITERAL NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// LiteralExprAST
//
// Any scalar literal value — numbers, strings, characters, booleans, nil.
//   42         →  kind=Int,    value="42"
//   3.14       →  kind=Float,  value="3.14"
//   "hello"    →  kind=String, value="hello"   (escape sequences already decoded)
//   r"a\nb"    →  kind=RawString, value="a\\nb"  (backslashes are literal)
//   'A'        →  kind=Char,   value="A"
//   0xFF       →  kind=Hex,    value="0xFF"
//   0b1010     →  kind=Binary, value="0b1010"
//   true       →  kind=True,   value="true"
//   false      →  kind=False,  value="false"
//   nil        →  kind=Nil,    value="nil"
//
// value stores the raw lexeme from the token. The semantic pass converts to
// a typed constant value during type checking.
// ─────────────────────────────────────────────────────────────────────────────

struct LiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::LiteralExpr;

    LiteralKind  kind;
    std::string  value;   // raw lexeme — "42", "3.14", "hello", "0xFF", ...

    LiteralExprAST(LiteralKind k, std::string v)
        : ExprAST(ASTKind::LiteralExpr), kind(k), value(std::move(v)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ArrayLiteralExprAST
//
// An array literal — a bracketed list of expressions.
//   [1, 2, 3]
//   ["hello", "world"]
//   []    — empty array literal
//
// The array kind (fixed / slice / dynamic) is inferred from the declared type
// of the variable being initialised — the literal itself is kind-neutral.
// The semantic pass sets resolvedType after inferring from context.
// ─────────────────────────────────────────────────────────────────────────────

struct ArrayLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayLiteralExpr;

    std::vector<ExprPtr> elements;   // may be empty

    ArrayLiteralExprAST() : ExprAST(ASTKind::ArrayLiteralExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// StructLiteralExprAST
//
// Constructs a value of a named struct type.
//   Vec2 { x = 1.0  y = 2.0 }
//   Color {}                      — all fields take their defaults
//   Pair<int, string> { first = 1  second = "one" }
//
// Field inits always use '=' — never ':'. The ':' only appears inside match
// struct patterns (handled in PatternAST.hpp).
//
// genericArgs is populated when the type is explicitly instantiated:
//   Pair<int, string> { ... }  →  genericArgs = [Int, String]
// It is empty for non-generic structs.
//
// inits maps field name → initialiser expression. Ordering is not significant
// — the semantic pass matches by name against the struct's field declarations.
// ─────────────────────────────────────────────────────────────────────────────

struct FieldInitAST {
    std::string  name;    // field name
    ExprPtr      value;   // initialiser expression
    SourceLocation loc;   // for error reporting
};

struct StructLiteralExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::StructLiteralExpr;

    std::string                      typeName;      // "Vec2", "Color", "Pair"
    std::vector<TypePtr>             genericArgs;   // empty if non-generic
    std::vector<FieldInitAST>        inits;         // field = expr entries

    StructLiteralExprAST() : ExprAST(ASTKind::StructLiteralExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// NAME & ACCESS NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// IdentifierExprAST
//
// A bare identifier used as an expression.
//   x        — local variable or parameter
//   add      — function name
//   Direction — enum type name (used before .North in Direction.North)
//
// The semantic pass resolves the name against the symbol table and sets
// resolvedType. If the name resolves to an enum type followed by '.', the
// parser actually produces a FieldAccessExprAST — an IdentifierExprAST always
// refers to a single symbol, never a qualified name.
// ─────────────────────────────────────────────────────────────────────────────

struct IdentifierExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IdentifierExpr;

    std::string name;

    explicit IdentifierExprAST(std::string n)
        : ExprAST(ASTKind::IdentifierExpr), name(std::move(n)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldAccessExprAST
//
// Access a data member (struct field) via the '.' operator.
//   v.x            →  object=IdentifierExprAST("v"),  field="x"
//   player.health  →  object=IdentifierExprAST("player"),  field="health"
//   Direction.North → object=IdentifierExprAST("Direction"), field="North"
//                     (enum variant access — field is a variant name)
//
// The '.' operator always means data — a field declared inside a struct body,
// or an enum variant. Impl methods use ':' (BehaviorAccessExprAST).
//
// The semantic pass checks that field exists on the resolved type of object
// and that any write to the field is through a 'let' variable.
// ─────────────────────────────────────────────────────────────────────────────

struct FieldAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::FieldAccessExpr;

    ExprPtr     object;   // the left-hand side expression
    std::string field;    // field name

    FieldAccessExprAST() : ExprAST(ASTKind::FieldAccessExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// BehaviorAccessExprAST
//
// Access an impl method via the ':' operator.
//   Vec2:normalize   →  typeName="Vec2",  method="normalize"
//   Vec2:length      →  typeName="Vec2",  method="length"
//
// Behavior members are never reassignable — the semantic pass enforces this
// via the isBehaviorMember flag inherited from BaseAST.
//
// The result is a plain function reference — it can be stored in a typed
// variable, passed as an argument, or used as a pipeline step.
//
// The semantic pass resolves typeName to a struct in the symbol table, then
// looks up method in its merged impl surface.
// ─────────────────────────────────────────────────────────────────────────────

struct BehaviorAccessExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BehaviorAccessExpr;

    std::string typeName;   // "Vec2", "Circle"
    std::string method;     // "normalize", "draw"

    BehaviorAccessExprAST() : ExprAST(ASTKind::BehaviorAccessExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// CALL & INDEX NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// CallExprAST
//
// A function call — regular or generic.
//   f(args)           →  callee=IdentifierExprAST("f"),  genericArgs={},  args=[...]
//   Vec2(boiling)     →  callee=IdentifierExprAST("Vec2"),  — from() dispatch at semantic
//   Buffer<int>(cap)  →  genericArgs=[Int],  args=[cap]
//   obj.method(args)  →  callee=FieldAccessExpr(obj,"method"),  args=[...]
//
// genericArgs is non-empty only when the source had explicit type arguments:
//   process<int>(x) → genericArgs=[Int]
// The semantic pass validates the arg count against the function's signature.
//
// isArgPack is true when the call was suffixed with '!' in a pipeline step:
//   fn(args)!  — upstream value will be injected as the first argument.
// The semantic pass checks that isArgPack is only true inside a pipeline step.
// ─────────────────────────────────────────────────────────────────────────────

struct CallExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::CallExpr;

    ExprPtr              callee;       // what is being called
    std::vector<TypePtr> genericArgs;  // explicit type args — empty if absent
    std::vector<ExprPtr> args;         // call arguments in order
    bool                 isArgPack = false; // true → fn(args)! pipeline argument pack

    CallExprAST() : ExprAST(ASTKind::CallExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// IndexExprAST
//
// Array element access or slice expression — the IndexKind enum (defined in
// BaseAST.hpp) distinguishes which form was written.
//
//   nums[2]      →  kind=Element,  target=nums,  index=2,  sliceEnd=nullptr
//   nums[1..3]   →  kind=Slice,    target=nums,  index=1,  sliceEnd=3
//
// For Element:  index holds the element index expression.  sliceEnd is null.
// For Slice:    index holds the start expression.  sliceEnd holds the end.
//   Both ends are inclusive — nums[1..3] contains elements at 1, 2, 3.
//
// Semantic rules enforced by the semantic pass:
//   Element — index must be a non-negative integer; out of bounds = runtime panic
//   Slice   — start >= 0, end >= start, both < array.len() at runtime
//   Write   — only valid when the target is a 'let' variable (imt/val = error)
// ─────────────────────────────────────────────────────────────────────────────

struct IndexExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IndexExpr;

    ExprPtr   target;     // the array being indexed
    ExprPtr   index;      // element index (Element) or slice start (Slice)
    ExprPtr   sliceEnd;   // slice end — nullptr for Element kind
    IndexKind kind;
    bool      isExclusive = false; // true if ..< syntax used

    IndexExprAST() : ExprAST(ASTKind::IndexExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// OPERATOR NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// BinaryExprAST
//
// Any infix binary operation — arithmetic, comparison, logical, bitwise.
//   a + b    →  op=Add,  left=a,  right=b
//   x == y   →  op=Eq,   left=x,  right=y
//   p and q  →  op=And,  left=p,  right=q
//   a | b    →  op=BitOr (in expression position — not a union type)
//
// Note: '|' is both the union type operator (TypeAST) and bitwise OR (ExprAST).
// The parser disambiguates by context — in a type position it produces a
// UnionTypeAST; in an expression position it produces BinaryExprAST(BitOr).
// ─────────────────────────────────────────────────────────────────────────────

struct BinaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::BinaryExpr;

    BinaryOp  op;
    ExprPtr   left;
    ExprPtr   right;

    BinaryExprAST() : ExprAST(ASTKind::BinaryExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// UnaryExprAST
//
// A prefix unary operation.
//   -x      →  op=Neg,    operand=x
//   not x   →  op=Not,    operand=x
//   ~x      →  op=BitNot, operand=x
//   &x      →  op=Ref,    operand=x   — take a reference
// ─────────────────────────────────────────────────────────────────────────────

struct UnaryExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnaryExpr;

    UnaryOp  op;
    ExprPtr  operand;

    UnaryExprAST() : ExprAST(ASTKind::UnaryExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// AssignExprAST
//
// An assignment — plain or compound.
//   x = 5        →  op=Assign,    lhs=x,  rhs=5
//   x += 1       →  op=AddAssign, lhs=x,  rhs=1
//   obj.health -= damage  →  op=SubAssign, lhs=obj.health, rhs=damage
//
// Compound operators desugar to  lhs = lhs op rhs  at semantic time.
// The semantic pass checks:
//   - lhs resolves to a 'let' variable or a 'let'-held field
//   - imt and val on lhs is an error
//   - operator is defined for the lhs type (e.g. -= on string is an error)
// ─────────────────────────────────────────────────────────────────────────────

struct AssignExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AssignExpr;

    AssignOp  op;
    ExprPtr   lhs;
    ExprPtr   rhs;

    AssignExprAST() : ExprAST(ASTKind::AssignExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// IsExprAST
//
// Runtime type check — produces bool and narrows the type in the enclosing block.
//   x is int              →  expr=x,     type=Int
//   shape is Circle       →  expr=shape, type=Circle
//   stage is ShaderStage.Fragment  →  expr=stage, type=NamedType("ShaderStage.Fragment")
//
// After  if x is SomeType { ... }:
//   - inside the block: x is treated as SomeType (direct field/method access)
//   - outside the block: x reverts to its declared type
//
// The semantic pass writes the narrowedType semantic flag on the node so that
// the type checker can use it when checking the branch body.
// ─────────────────────────────────────────────────────────────────────────────

struct IsExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IsExpr;

    ExprPtr  expr;
    TypePtr  checkType;   // the type being checked against

    IsExprAST() : ExprAST(ASTKind::IsExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// NULLABLE CHAIN NODE
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// NullableChainExprAST
//
// A .? chain terminated by a ?? fallback.
//   player.?weapon.?damage ?? 0
//   getSession(token).?user.?profile.?displayName ?? "anonymous"
//
// The grammar enforces that every .? chain MUST be terminated by ??.
// Standalone '.' (non-nullable field access) never needs ?? and never
// produces a NullableChainExprAST — it becomes a FieldAccessExprAST.
//
// object  — the root expression before the first .?
// steps   — the field names accessed via .? in order
//   e.g. player.?weapon.?damage  →  object=player, steps=["weapon","damage"]
// fallback — the expression after ??
//
// The semantic pass verifies that:
//   - every field in steps exists on the corresponding nullable type
//   - fallback type is compatible with the final resolved type of the chain
// ─────────────────────────────────────────────────────────────────────────────

struct NullableChainExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::NullableChainExpr;

    ExprPtr                  object;    // root expression
    std::vector<std::string> steps;     // field names accessed via .?
    ExprPtr                  fallback;  // the ?? value

    NullableChainExprAST() : ExprAST(ASTKind::NullableChainExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// PIPELINE & COMPOSITION NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// PipelineStepAST
//
// One step in a pipeline chain — not an ExprAST node itself because it cannot
// appear independently as an expression. It is always owned by PipelineExprAST.
//
// Five syntactic forms exist (PipelineStepKind):
//
//   Ident       fn                   — plain function name
//   BehaviorRef Type:method          — impl method reference
//   FieldRef    obj.field            — data field of non-nullable function type
//   ArgPack     fn(args)!            — argument pack, upstream injected first
//   AnonFunc    (x T) R { ... }      — inline anonymous function step
//
// Depending on kind, different fields are populated:
//   Ident:       ident filled
//   BehaviorRef: typeName + method filled
//   FieldRef:    ident (obj) + field filled
//   ArgPack:     ident (fn) + packArgs filled
//   AnonFunc:    anonFunc filled
// ─────────────────────────────────────────────────────────────────────────────

struct PipelineStepAST {
    PipelineStepKind          kind;

    // Ident / FieldRef / ArgPack — the base identifier
    std::string               ident;

    // BehaviorRef — Type:method
    std::string               typeName;   // "Vec2"
    std::string               method;     // "normalize"

    // FieldRef — obj.field
    std::string               field;

    // ArgPack — fn(args)!
    std::vector<ExprPtr>      packArgs;   // the args inside fn(args)!

    // AnonFunc — inline anonymous function
    std::unique_ptr<struct AnonFuncExprAST> anonFunc;  // forward declared below

    SourceLocation            loc;
};

using PipelineStepPtr = std::unique_ptr<PipelineStepAST>;

// ─────────────────────────────────────────────────────────────────────────────
// PipelineExprAST
//
// A runtime pipeline chain — seed -> step -> step -> ...
//   42 -> float -> sqrt
//   getUser(id) -> validate -> save
//   v -> Vec2:normalize -> scale(2.0)!
//
// seed — any expression: variable, literal, function call result, arithmetic.
// steps — one or more pipeline steps in order.
//
// The pipeline short-circuits on Error when the error library is used:
// if a step returns Error and the next step doesn't accept Error, the error
// is forwarded to the end of the chain where ?? or .catch handles it.
//
// The semantic pass verifies each step is callable and that non-nullable
// rules are respected for FieldRef steps.
// ─────────────────────────────────────────────────────────────────────────────

struct PipelineExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::PipelineExpr;

    ExprPtr                      seed;
    std::vector<PipelineStepPtr> steps;   // at least one

    PipelineExprAST() : ExprAST(ASTKind::PipelineExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ComposeOperandAST
//
// One operand in a +> composition chain — not an ExprAST because it cannot
// appear independently. Always owned by ComposeExprAST.
//
// Three syntactic forms (ComposeOperandKind) — no ArgPack, no AnonFunc:
//   Ident:       ident filled
//   BehaviorRef: typeName + method filled
//   FieldRef:    ident (obj) + field filled  — non-nullable only
//
// The semantic pass checks that each operand is non-nullable and that the
// output type of the left operand exactly matches the input type of the right.
// ─────────────────────────────────────────────────────────────────────────────

struct ComposeOperandAST {
    ComposeOperandKind  kind;

    std::string         ident;      // Ident / FieldRef
    std::string         typeName;   // BehaviorRef — "Vec2"
    std::string         method;     // BehaviorRef — "normalize"
    std::string         field;      // FieldRef

    SourceLocation      loc;
};

using ComposeOperandPtr = std::unique_ptr<ComposeOperandAST>;

// ─────────────────────────────────────────────────────────────────────────────
// ComposeExprAST
//
// A compile-time function composition chain — f +> g +> h
//   let process = validate +> transform +> render
//
// left is a pipeline expression (the left side of the first +>).
// operands holds the right-hand operands in order.
//
// +> enforces strict type matching at compile time:
//   output type of left must exactly match input type of first operand
//   output type of operand[n] must exactly match input type of operand[n+1]
//
// The semantic pass validates these type chains. Generics must be explicitly
// instantiated before composing — type inference across +> is not supported.
// ─────────────────────────────────────────────────────────────────────────────

struct ComposeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ComposeExpr;

    ExprPtr                        left;       // left-hand pipeline expression
    std::vector<ComposeOperandPtr> operands;   // right-hand operands in order

    ComposeExprAST() : ExprAST(ASTKind::ComposeExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// FUNCTION NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// AnonFuncExprAST
//
// An anonymous function expression — a function value without a name.
// Mirrors FuncDeclAST exactly in its signature shape so that curried anon
// functions are first-class and can be assigned to curried let variables.
//
// Single-group (normal function):
//   (x int) int { return x * 2 }
//   async (url string) string { return await httpGet(url) }
//   () { io.printl("done") }
//
// Multi-group (curried anonymous function):
//   (a int) (b int) int { return a + b }
//   (min int) (max int) (value int) int { ... }
//
// Used in:
//   - Explicit function assignment:  let f (x int) int = (x int) int { ... }
//   - Curried reassignment:          let add (a int)(b int) int = ...
//                                    add = (a int)(b int) int { return a + b }
//   - Pipeline steps:                f1(args) -> (result int) string { ... } -> io.printl
//   - Inline callbacks:              nums -> array.filter((x int) bool { x > 3 })
//
// paramGroups — outer vector = curry groups (same layout as FuncDeclAST).
//   Single group  → normal function, paramGroups.size() == 1.
//   Multiple groups → curried function, paramGroups.size() > 1.
//   Empty outer vector (paramGroups.empty()) is the bare block-body marker
//   produced by the parser when it sees a plain '{' in expression position;
//   the enclosing FuncDeclAST owns the real signature in that case.
//
// returnType is nullptr for void anonymous functions.
// isAsync marks async anonymous functions.
// body is always a BlockStmtAST.
// ─────────────────────────────────────────────────────────────────────────────

struct AnonFuncExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AnonFuncExpr;

    std::vector<std::vector<ParamPtr>> paramGroups;  // outer = curry groups
    TypePtr                            returnType;   // nullptr = void
    StmtPtr                            body;         // BlockStmtAST
    bool                               isAsync = false;

    AnonFuncExprAST() : ExprAST(ASTKind::AnonFuncExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// AwaitExprAST
//
// Suspends the current async function until the awaited future resolves.
//   await httpGet(url)
//   await fetchAll(items)
//
// Only valid inside an async function body — the semantic pass reports an
// error if await appears outside of one.
// Also invalid inside parallel for and parallel block bodies.
// ─────────────────────────────────────────────────────────────────────────────

struct AwaitExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::AwaitExpr;

    ExprPtr inner;   // the async expression being awaited

    explicit AwaitExprAST(ExprPtr e)
        : ExprAST(ASTKind::AwaitExpr), inner(std::move(e)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// CONTROL FLOW EXPRESSION NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// IfExprAST
//
// The expression form of if — both branches required, both produce the same type.
//   if n >= 0 ?? "positive" else "negative"
//   if score >= 60 ?? "pass" else "fail"
//
// This is distinct from IfStmtAST (in StmtAST.hpp) where else is optional and
// no value is produced. The parser selects the correct node based on context:
//   - after '=' in a func body          → IfExprAST
//   - in any expression position        → IfExprAST
//   - as a standalone statement         → IfStmtAST
//
// Both then and elseBranch are StmtPtr (BlockStmtAST) — the same block type
// used everywhere. The semantic pass checks that both branches produce the
// same type and that elseBranch is present.
// ─────────────────────────────────────────────────────────────────────────────

struct IfExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::IfExpr;

    ExprPtr  condition;
    ExprPtr  thenBranch;    // expression
    ExprPtr  elseBranch;    // expression

    IfExprAST() : ExprAST(ASTKind::IfExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// OTHER EXPRESSION NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// RangeExprAST
//
// An inclusive range literal — lo..hi.
//   0..10     — used in for loops: for i in 0..10
//   1..10     — used in match range patterns: case 1..10
//   1..3      — used in slice index: nums[1..3]
//
// Both ends are inclusive — 0..10 iterates 0,1,2,...,10 (11 steps).
// The parser produces this node wherever '..' appears between two expressions.
// The semantic pass checks the usage context to enforce the correct rules:
//   - for loop: lo and hi must be integers
//   - match pattern: lo and hi must be integer literals
//   - slice index: both must be >= 0 and end >= start
// ─────────────────────────────────────────────────────────────────────────────

struct RangeExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::RangeExpr;

    ExprPtr lo;   // start (inclusive)
    ExprPtr hi;   // end   (inclusive/exclusive depends on flag)
    bool isExclusive = false; // true for ..<

    RangeExprAST() : ExprAST(ASTKind::RangeExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeConvExprAST
//
// An explicit type cast expression — safe primitive cast or unsafe bit
// reinterpret (FFI / Vulkan only).
//
// Safe cast (isUnsafe = false):
//   float(x)       — int → float via standard widening
//   string(n)      — int → string via formatting
//   int(direction) — enum → underlying integer
//
// Unsafe bit reinterpret (isUnsafe = true):
//   *float(bits)   — reinterpret uint32 bits as float32, no arithmetic
//   *GpuVertex(raw)— reinterpret raw memory as GpuVertex struct
//
// targetType — the type being cast to (TypeAST node).
// expr — the expression whose value is being cast.
//
// The semantic pass enforces:
//   - safe: only valid cast paths are allowed (primitive widening, enum→int)
//   - unsafe (*): only valid inside extern declaration subtrees
// ─────────────────────────────────────────────────────────────────────────────

struct TypeConvExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::TypeConvExpr;

    TypePtr  targetType;   // the type to convert to
    ExprPtr  expr;         // the value being converted
    bool     isUnsafe;     // true → *T(expr) bit reinterpret

    TypeConvExprAST(TypePtr t, ExprPtr e, bool unsafe = false)
        : ExprAST(ASTKind::TypeConvExpr),
          targetType(std::move(t)), expr(std::move(e)), isUnsafe(unsafe) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ═════════════════════════════════════════════════════════════════════════════
// PATTERN NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// LiteralPatternAST — REMOVED
//
// Previously matched a literal value in pattern position (42, "ok", true, nil).
// Replaced by LiteralExprAST placed directly into MatchArmAST::patterns.
// The semantic pass recognises ASTKind::LiteralExpr in pattern position and
// applies match semantics (equality test against the subject) rather than
// evaluation semantics.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// RangePatternAST — REMOVED
//
// Previously matched a range in pattern position (1..10, 11..<50).
// Replaced by RangeExprAST placed directly into MatchArmAST::patterns.
// The semantic pass recognises ASTKind::RangeExpr in pattern position and
// applies match semantics (inclusive/exclusive bounds test against the subject).
// RangeExprAST::isExclusive covers the ..<  form.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// BindPatternAST
//
// Matches any value and binds it to a name for use in the guard and arm body.
//   n            — bind without guard: captures matched value as 'n'
//   n if n < 50  — bind with guard: only matches when n < 50
//   arr          — bind array: arr.len() usable in guard and body
//
// The bound name is declared by the pattern itself — it is not looked up
// in an outer scope. The semantic pass introduces 'n' (or whatever the name
// is) as a new variable in the arm's scope with the type of the match subject.
//
// A BindPatternAST without a guard on its arm matches everything. The
// semantic pass enforces that an unconditional bind must come after all
// more-specific patterns (literal, range, type, struct) — otherwise
// subsequent arms would be unreachable.
//
// Note: the guard lives on MatchArmAST, not on the pattern. A single arm
// with a bind pattern has at most one guard. The pattern itself is just
// the name binding.
// ─────────────────────────────────────────────────────────────────────────────

struct BindPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::BindPattern;

    std::string name;   // "n", "arr", "s", "v"

    explicit BindPatternAST(std::string n)
        : PatternAST(ASTKind::BindPattern), name(std::move(n)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// WildcardPatternAST
//
// Matches any value and discards it — the '_' token.
//   _       -> "non-zero"
//
// Semantically identical to BindPatternAST except no name is introduced
// into scope. A wildcard pattern may appear with a guard, but since the
// value is discarded the guard cannot reference the matched value by name.
//
// '_' and 'default' are distinct:
//   _       — a pattern that matches anything and discards the value.
//             May appear in a normal arm (with or without a guard).
//   default — the required final fallback arm keyword. Not a pattern.
//
// The semantic pass enforces that _ does not appear as the last pattern
// before 'default' in a way that makes 'default' unreachable.
// ─────────────────────────────────────────────────────────────────────────────

struct WildcardPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::WildcardPattern;

    WildcardPatternAST() : PatternAST(ASTKind::WildcardPattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypePatternAST
//
// Matches when the subject is of a specific type, and binds the narrowed
// value to a name. Combines a bind and a runtime type check in one pattern.
//   s is Circle    — matches if subject is Circle, binds as 's' typed Circle
//   v is Rect      — matches if subject is Rect,   binds as 'v' typed Rect
//   e is Error     — matches if subject is Error,  binds as 'e' typed Error
//
// Grammar: IDENTIFIER 'is' type
//   bindName  — the name introduced into the arm's scope
//   checkType — the type being tested against
//
// After a successful match the semantic pass narrows bindName's type to
// checkType for the duration of the arm body. Outside the arm the original
// subject type is unchanged.
//
// Used for union type dispatch and 'any' type dispatch:
//   type Shape = Circle | Rect | Triangle
//   match shape {
//       s is Circle   -> s.radius * s.radius * 3.14159
//       s is Rect     -> s.width * s.height
//       default       -> 0.0
//   }
// ─────────────────────────────────────────────────────────────────────────────

struct TypePatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::TypePattern;

    std::string  bindName;    // "s", "v", "e" — introduced into arm scope
    TypePtr      checkType;   // Circle, Rect, Error, ...

    TypePatternAST() : PatternAST(ASTKind::TypePattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldPatternAST
//
// One field entry inside a struct pattern — a plain struct, not a PatternAST.
// Never appears independently — always owned by StructPatternAST.
//
// Two syntactic forms:
//   x        — shorthand: matches field 'x' and binds it to name 'x'
//              field="x",  subPattern=nullptr  (bind name = field name)
//   x: 0.0   — full form: matches field 'x' against sub-pattern 0.0
//              field="x",  subPattern=LiteralExprAST(0.0)
//   x: Vec2 { ... } — nested: field 'x' matched against a nested struct pattern
//              field="x",  subPattern=StructPatternAST(...)
//
// When subPattern is nullptr the field is bound by name (shorthand form).
// When subPattern is present the field value must match the sub-pattern.
// subPattern is BaseAST* for the same reason as MatchArmAST::patterns —
// literal sub-patterns are LiteralExprAST (ExprAST), while structural
// sub-patterns are PatternAST; no common subtype tighter than BaseAST exists.
//
// Note: struct patterns use ':' as the field separator — this is pattern
// syntax only. Struct literals always use '=' (never ':').
// ─────────────────────────────────────────────────────────────────────────────

struct FieldPatternAST {
    std::string                  field;        // field name from the struct
    std::unique_ptr<BaseAST>     subPattern;   // nullptr → shorthand bind by name
    SourceLocation               loc;
};

using FieldPatternPtr = std::unique_ptr<FieldPatternAST>;

// ─────────────────────────────────────────────────────────────────────────────
// StructPatternAST
//
// Matches when the subject is a struct of the named type and its fields
// satisfy the given field patterns.
//
//   Vec2 { x: 0.0, y: 0.0 }   — exact match on both fields
//   Vec2 { x, y }              — shorthand: binds x and y from subject
//   Player { health: 0 }       — matches only when health == 0, other fields ignored
//   Player { pos: Vec2 { x: 0.0, y: 0.0 }, health }  — nested pattern
//
// typeName   — the struct type name ("Vec2", "Player")
// fields     — the field patterns in source order
//
// Fields not listed in the pattern are ignored — the match succeeds as long
// as the listed fields satisfy their patterns. This is intentional: you don't
// have to list every field.
//
// The semantic pass verifies:
//   - typeName resolves to a struct in the symbol table
//   - every listed field name exists on that struct
//   - sub-pattern types are compatible with the field types
// ─────────────────────────────────────────────────────────────────────────────

struct StructPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::StructPattern;

    std::string                   typeName;  // "Vec2", "Player"
    std::vector<FieldPatternPtr>  fields;    // field patterns in source order

    StructPatternAST() : PatternAST(ASTKind::StructPattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};


// ═════════════════════════════════════════════════════════════════════════════
// ARM NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// MatchArmAST
//
// One non-default arm in a match expression.
//   200           -> "ok"
//   200, 201, 202 -> "success"
//   1..10         -> "light"
//   n if n < 0    -> "invalid: " + string(n)
//   s is Circle   -> s.radius * s.radius * 3.14159
//   Vec2 { x, y } -> "at " + string(x) + ", " + string(y)
//
// patterns — one or more patterns, comma-separated in source. Stored as
//   vector<unique_ptr<BaseAST>> rather than vector<unique_ptr<PatternAST>>
//   because literal and range arms now place LiteralExprAST and RangeExprAST
//   nodes here directly — those inherit from ExprAST, not PatternAST, so there
//   is no common subtype tighter than BaseAST. The semantic pass uses isa<> /
//   as<> on ASTKind to discriminate each node; no compile-time narrower type
//   is available or necessary.
//   Valid ASTKind values in this vector:
//     LiteralExpr     — literal value pattern: 42, "ok", true
//     RangeExpr       — range pattern: 1..10, 1..<10
//     BindPattern     — bind pattern: n
//     WildcardPattern — discard pattern: _
//     TypePattern     — type + bind pattern: v is Circle
//     StructPattern   — destructure pattern: Vec2 { x, y }
//   All patterns in the list are tried — the arm fires if any matches.
//   Multiple patterns share the same guard and body.
//   Constraint: all patterns in a list must bind the same set of names
//   so the body can reference them unambiguously — enforced by the semantic pass.
//
// guard — optional filter expression, only valid after a bind or wildcard pattern.
//   nullptr means no guard — the arm fires on pattern match alone.
//   The guard expression may reference names introduced by bind patterns.
//
// exprs — one or two comma-separated result expressions (primary, optional secondary).
//   The expressions may reference names introduced by bind patterns in this arm.
//   See secondary value rules in the grammar for nullable semantics when only
//   some arms supply a secondary value.
//
// Semantic pass ordering rules:
//   - Arms are tested top to bottom — order matters
//   - An unconditional bind (no guard) matches everything;
//     any arm after it is unreachable — semantic error
//   - Wildcard without guard is also unreachable if not last before default
// ─────────────────────────────────────────────────────────────────────────────

struct MatchArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MatchArm;

    std::vector<std::unique_ptr<BaseAST>> patterns;   // at least one — see comment above
    ExprPtr                               guard;       // nullptr if no guard
    std::vector<ExprPtr>                  exprs;       // 1 or more result expressions

    MatchArmAST() : BaseAST(ASTKind::MatchArm) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using  MatchArmPtr = std::unique_ptr<MatchArmAST>;

// ─────────────────────────────────────────────────────────────────────────────
// DefaultArmAST
//
// The required final fallback arm — always present on every match expression.
//   default -> "unknown"
//   default -> "unknown", "no detail available"
//
// The 'default' arm has no pattern and no guard — it always matches.
// It must be the last arm in the match. The semantic pass reports an error
// if any arm follows default (unreachable arm) or if default is absent
// (non-exhaustive match).
//
// exprs — one or two comma-separated result expressions (primary, optional
//   secondary). Must be consistent with the secondary value presence across
//   all other arms — enforced by the semantic pass.
// ─────────────────────────────────────────────────────────────────────────────

struct DefaultArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::DefaultArm;

    std::vector<ExprPtr> exprs;

    DefaultArmAST() : BaseAST(ASTKind::DefaultArm) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using  DefaultArmPtr = std::unique_ptr<DefaultArmAST>;

// ─────────────────────────────────────────────────────────────────────────────
// MatchExprAST
//
// Pattern matching expression — always produces a value.
//   match status {
//       200      -> "ok"
//       404      -> "not found"
//       default  -> "unknown"
//   }
//
// subject — the expression being matched.
// arms — the non-default match arms in source order.
// defaultBody — the required default arm body (expr or block).
//   defaultLoc — source location of the 'default' keyword.
//
// Grammar rules enforced by the semantic pass:
//   - default arm is required and must be last
//   - all arms must produce the same type
//   - wildcards and unconditional binds must appear before default
//   - enum exhaustiveness may be checked when subject is an enum type
//
// defaultBody is stored as a DefaultArmPtr (DefaultArmAST) which contains
// the executable body (exprs) of the fallback case.
// ─────────────────────────────────────────────────────────────────────────────

struct MatchExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::MatchExpr;

    ExprPtr                  subject;
    std::vector<MatchArmPtr> arms;
    DefaultArmPtr            defaultBody;   // required — body of the default arm
    SourceLocation           defaultLoc;    // location of the 'default' keyword

    MatchExprAST() : ExprAST(ASTKind::MatchExpr) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};