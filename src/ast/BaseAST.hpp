/**
 * @file BaseAST.hpp
 *
 * @responsibility The Foundation. Defines the BaseAST, the Visitor interface,
 *                 and common types (DocComment, SourceLocation, ASTKind).
 *
 * @architectural_note
 *   This file uses Forward Declarations for all AST families (Expr, Stmt, etc.).
 *   NEVER include a family header (like ExprAST.hpp) here; this keeps the
 *   dependency graph acyclic.
 *
 * @related_files
 *   - src/ast/ExprAST.hpp, StmtAST.hpp, DeclAST.hpp, TypeAST.hpp
 *   - Each family header includes BaseAST.hpp, not the other way around.
 */

#pragma once

#include "debug/DebugMacros.hpp"
#include "support/ASTArena.hpp"
#include "support/InternedString.hpp"
#include "support/ArenaSpan.hpp"

#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations — every AST family forward-declared here so any header
// can accept a visitor or hold a pointer without pulling in the full family.
//
// The actual struct definitions live in their own headers:
//   TypeAST.hpp     — PrimitiveTypeAST, NamedTypeAST, FixedArrayTypeAST, ...
//   DeclAST.hpp     — FuncDeclAST, StructDeclAST, ImplDeclAST, ...
//   ExprAST.hpp     — LiteralExprAST, CallExprAST, PipelineExprAST, ...
//   StmtAST.hpp     — BlockStmtAST, ForStmtAST, ...
// ─────────────────────────────────────────────────────────────────────────────

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct NullableTypeAST;
struct ResultTypeAST;        // T!E / T! — result type
struct FixedArrayTypeAST;
struct SliceTypeAST;
struct DynamicArrayTypeAST;
struct RefTypeAST;
struct PtrTypeAST;
struct FuncTypeAST;

// DeclAST.hpp
struct PackageDeclAST;
struct UseDeclAST;
struct VarDeclAST;
struct ParamAST;
struct GenericParamAST;
struct FuncDeclAST;
struct FieldDeclAST;
struct StructDeclAST;
struct EnumVariantAST;
struct EnumDeclAST;
struct TraitMethodAST;
struct TraitDeclAST;
struct TraitRefAST;
struct MethodDeclAST;
struct FromDeclAST;
struct FromEntryAST;
struct ImplDeclAST;
struct TypeAliasDeclAST;

// ExprAST.hpp
struct LiteralExprAST;
struct IdentifierExprAST;
struct ArrayLiteralExprAST;
struct StructLiteralExprAST;
struct FieldInitAST;
struct BinaryExprAST;
struct UnaryExprAST;
struct CallExprAST;
struct IndexExprAST;
struct FieldAccessExprAST;
struct BehaviorAccessExprAST;
struct NullableChainExprAST;
struct NullCoalesceExprAST;
struct AssignExprAST;
struct IsExprAST;
struct PipelineExprAST;
struct PipelineStepAST;
struct ComposeExprAST;
struct ComposeOperandAST;
struct AnonFuncExprAST;
struct AwaitExprAST;
struct ResolveExprAST;       // resolve expr { ok ... err ... }
struct OkArmAST;             // ok (v T) { ... }
struct ErrArmAST;            // err (e E) { ... }
struct MatchExprAST;
struct IfExprAST;
struct RangeExprAST;
struct TypeConvExprAST;

// Pattern nodes (defined in ExprAST.hpp)
struct BindPatternAST;
struct WildcardPatternAST;
struct TypePatternAST;
struct StructPatternAST;
struct FieldPatternAST;
struct PatternExprAST;
struct MatchArmAST;
struct DefaultArmAST;

// StmtAST.hpp
struct BlockStmtAST;
struct ExprStmtAST;
struct DeclStmtAST;
struct IfStmtAST;
struct SwitchStmtAST;
struct SwitchCaseAST;
struct ForStmtAST;
struct WhileStmtAST;
struct DoWhileStmtAST;
struct ReturnStmtAST;
struct BreakStmtAST;
struct ContinueStmtAST;
struct MultiVarDeclAST;
struct MultiAssignStmtAST;

// Root
struct ProgramAST;

// Unknown nodes
struct UnknownDeclAST;
struct UnknownExprAST;
struct UnknownStmtAST;
struct UnknownTypeAST;

// Compiler Directive nodes
struct AttributeAST;
struct AttributeArgAST;
struct IntrinsicCallExprAST;

// ─────────────────────────────────────────────────────────────────────────────
// ASTKind — compile-time tag stored on every node.
//
// Replaces runtime RTTI / dynamic_cast with a single integer comparison.
// Every concrete node defines `static constexpr ASTKind staticKind` and passes
// it to the BaseAST constructor.
//
// Usage:
//   if (node->kind == ASTKind::PrimitiveType) {
//       auto* p = static_cast<PrimitiveTypeAST*>(node);
//   }
//
// Or use the helpers on BaseAST:
//   if (node->isa<PrimitiveTypeAST>()) { node->as<PrimitiveTypeAST>() ... }
// ─────────────────────────────────────────────────────────────────────────────
enum class ASTKind : uint16_t {
    Unknown,
    UnknownDecl,
    UnknownExpr,
    UnknownStmt,
    UnknownType,

    // Type nodes
    PrimitiveType,
    NamedType,
    NullableType,
    ResultType,      // T!E / T! — result type
    ArrayType,
    GenericArrayType,
    RefType,
    PtrType,
    FuncType,

    // Declaration nodes
    PackageDecl,
    UseDecl,
    VarDecl,
    Param,
    GenericParam,
    FuncDecl,
    FieldDecl,
    StructDecl,
    EnumVariant,
    EnumDecl,
    TraitMethod,
    TraitDecl,
    TraitRef,
    MethodDecl,
    FromDecl,
    FromEntry,
    ImplDecl,
    TypeAliasDecl,

    // Expression nodes
    CallableRefExpr,
    LiteralExpr,
    ArrayLiteralExpr,
    StructLiteralExpr,
    FieldInit,
    IdentifierExpr,
    FieldAccessExpr,
    BehaviorAccessExpr,
    CallExpr,
    IndexExpr,
    BinaryExpr,
    UnaryExpr,
    AssignExpr,
    IsExpr,
    NullableChainExpr,
    NullCoalesceExpr,
    PipelineExpr,
    PipelineStep,
    ComposeExpr,
    ComposeOperand,
    AnonFuncExpr,
    AwaitExpr,
    ResolveExpr,     // resolve expr { ok ... err ... }
    OkArm,           // ok (v T) { ... }
    ErrArm,          // err (e E) { ... }
    MatchExpr,
    IfExpr,
    RangeExpr,
    TypeConvExpr,

    // Statement nodes
    BlockStmt,
    ExprStmt,
    DeclStmt,
    IfStmt,
    SwitchStmt,
    SwitchCase,
    ForStmt,
    WhileStmt,
    DoWhileStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    MultiVarDecl,
    MultiAssignStmt,

    // Pattern nodes
    BindPattern,
    WildcardPattern,
    TypePattern,
    StructPattern,
    FieldPattern,
    PatternExpr,
    MatchArm,
    DefaultArm,

    // Root
    Program,

    // Compiler directives
    Attribute,
    AttributeArg,
    IntrinsicCallExpr,
};

// ─────────────────────────────────────────────────────────────────────────────
// IndexKind — distinguishes array element access vs slice access.
// ─────────────────────────────────────────────────────────────────────────────
enum class IndexKind {
    Element,  // expr '[' expr ']'              — nums[2]
    Slice,    // expr '[' expr '..' expr ']'    — nums[1..3] (inclusive)
};

// ─────────────────────────────────────────────────────────────────────────────
// DocComment — documentation attached to declarations only (stored in DeclAST).
// ─────────────────────────────────────────────────────────────────────────────

enum class DocCommentForm {
    Stacked,   // consecutive '--' lines above declaration
    Block,     // /-- ... --/ block above declaration
    Trailing,  // '--' comment on same line as declaration
};

struct DocComment {
    InternedString  text;   // Markdown content, with ' -' prefix already stripped
    DocCommentForm  form;
};

// ─────────────────────────────────────────────────────────────────────────────
// SourceLocation — packed into 32 bits (20 bits line, 12 bits column).
// File path is stored once in ProgramAST, not per node.
// ─────────────────────────────────────────────────────────────────────────────

struct SourceLocation {
    uint32_t value = 0;  // line in high 20 bits, column in low 12 bits

    SourceLocation() = default;
    SourceLocation(uint32_t line, uint32_t column) {
        value = (line << 12) | (column & 0xFFF);
    }

    uint32_t line()   const { return value >> 12; }
    uint32_t column() const { return value & 0xFFF; }
    bool isKnown()    const { return value > 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// BaseAST — root of the entire AST hierarchy.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Root of the entire AST hierarchy. All concrete AST nodes inherit from this.
 *
 * This struct is deliberately small (typically 16-24 bytes on 64-bit platforms)
 * to minimise memory footprint across thousands of nodes. Node‑specific data
 * (doc comments, attributes, resolved types) is pushed down to family bases
 * (DeclAST, ExprAST, etc.).
 *
 * @par Memory Layout (64-bit, typical)
 *   - `kind` (ASTKind)        : 2 bytes  (uint16_t)
 *   - `loc` (SourceLocation)  : 4 bytes  (uint32_t)
 *   - `isBehaviorMember`      : 1 byte   (bool)
 *   - `isConst`               : 1 byte   (bool)
 *   - padding                 : 4 bytes  (alignment to 8 bytes)
 *   - vtable pointer          : 8 bytes
 *   @n Total: ~20 bytes (may vary by compiler and padding)
 *
 * @note `scopeDepth` and `effectFlags` were removed from this struct because
 *       they are rarely used and can be stored in semantic analysis side tables
 *       (e.g., ScopeTree, EffectAnalyzer) rather than bloating every AST node.
 *       This reduces per‑node memory by ~8 bytes.
 *
 * @field kind             Discriminator for LLVM‑style RTTI (isa/as). Zero overhead
 *                         compared to dynamic_cast.
 * @field loc              Packed source location (20 bits line, 12 bits column).
 *                         File path is stored once in ProgramAST, not per node.
 * @field isBehaviorMember Set by semantic pass for `Type:method` references.
 *                         Signals that this expression refers to an impl method.
 * @field isConst          Set by semantic pass for compile‑time constant values.
 *                         Used for constant folding and propagation.
 *
 * @par Usage
 *   Every concrete AST node inherits from BaseAST and provides:
 *     1. A `static constexpr ASTKind staticKind` member
 *     2. A constructor that passes `staticKind` to BaseAST
 *     3. (Optional) Semantic annotation fields
 *
 *   Type checking uses the `kind` field directly:
 *   @code
 *   switch (node->kind) {
 *       case ASTKind::IfStmt:
 *           auto* stmt = node->as<IfStmtAST>();
 *           // ...
 *   }
 *   @endcode
 *
 * @note Virtual destructor is required for polymorphic deletion when using
 *       raw pointers. With arena allocation, deletion never happens, but the
 *       vtable is still needed for `isa<>` and `as<>` to work correctly.
 */
struct BaseAST {
    ASTKind kind;
    SourceLocation loc;

    explicit BaseAST(ASTKind k) : kind(k) {}
    virtual ~BaseAST() = default;

    // Kind‑based type checking (no RTTI overhead)
    template<typename T>
    bool isa() const { return kind == T::staticKind; }

    template<typename T>
    T* as() {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>()");
        return static_cast<T*>(this);
    }

    template<typename T>
    const T* as() const {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>()");
        return static_cast<const T*>(this);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeArgKind — discriminator for attribute argument literals.
// ─────────────────────────────────────────────────────────────────────────────

enum class AttributeArgKind {
    StringLit,   // "string"
    IntLit,      // 42, 0xFF, 0b1010
    BoolLit,     // true, false
    TypeIdent    // TypeName (e.g., @extern("malloc", C))
};

struct AttributeArgAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::AttributeArg;

    AttributeArgKind kind;
    InternedString   value;   // raw source text

    AttributeArgAST(AttributeArgKind k, InternedString v)
        : BaseAST(ASTKind::AttributeArg), kind(k), value(v) {}

};

using AttributeArgPtr = ASTPtr<AttributeArgAST>;

struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    InternedString name;
    ArenaSpan<AttributeArgPtr> args;   // arguments, if any

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
};

using AttributePtr = ASTPtr<AttributeAST>;

// ─────────────────────────────────────────────────────────────────────────────
// Family bases – these add minimal data to enable heterogeneous collections.
// They forward the kind tag to BaseAST.
// ─────────────────────────────────────────────────────────────────────────────

struct TypeAST    : BaseAST { explicit TypeAST(ASTKind k)    : BaseAST(k) {} };

struct DeclAST    : BaseAST {
    std::optional<DocComment> doc;           // documentation comment
    ArenaSpan<AttributePtr>   attributes;    // compiler directives

    explicit DeclAST(ASTKind k) : BaseAST(k) {}
    bool hasDoc() const { return doc.has_value(); }
};

struct ExprAST    : BaseAST {
    TypeAST* resolvedType = nullptr;   // set by semantic pass

    // Semantic annotations specific to expressions
    bool isBehaviorMember = false;     // true for obj:method references
    bool isConst          = false;     // true for compile‑time constants

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};

struct StmtAST    : BaseAST { explicit StmtAST(ASTKind k) : BaseAST(k) {} };

struct PatternAST : BaseAST {
    TypeAST* resolvedType = nullptr;   // set by semantic pass

    explicit PatternAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Ownership aliases – each pointer uses ASTDeleter (no‑op) because nodes are
// arena‑allocated and freed in bulk.
// ─────────────────────────────────────────────────────────────────────────────

using TypePtr    = std::unique_ptr<TypeAST, ASTDeleter>;
using DeclPtr    = std::unique_ptr<DeclAST, ASTDeleter>;
using ExprPtr    = std::unique_ptr<ExprAST, ASTDeleter>;
using StmtPtr    = std::unique_ptr<StmtAST, ASTDeleter>;
using PatternPtr = std::unique_ptr<PatternAST, ASTDeleter>;

// ─────────────────────────────────────────────────────────────────────────────
// ProgramAST — root node for a single translation unit.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Root node for a single translation unit (source file).
 *
 * This node represents an entire `.luc` file after parsing. It owns all
 * top‑level declarations and provides file‑level context for semantic passes.
 *
 * @par Memory Layout (64-bit, typical)
 *   - BaseAST overhead        : ~20 bytes (vtable + kind + loc + flags + padding)
 *   - `packageName`           : 4 bytes (InternedString is a uint32_t)
 *   - `filePath`              : 4 bytes (InternedString)
 *   - `decls` (ArenaSpan)     : 16 bytes (ptr + size, each 8 bytes)
 *   @n Total: ~44 bytes per file (excluding the actual declaration nodes)
 *
 * @note Why separate `packageName` and `filePath`?
 *   - `packageName` is the identifier after `package` (e.g., "math").
 *     Used for cross‑file symbol resolution within the same package.
 *   - `filePath` is the relative path from the package root (e.g., "math/vec2.luc").
 *     Used for error messages, debug info, and module identity.
 *   Both are interned to avoid duplicate string storage across the AST.
 *
 * @par Declaration Ownership
 *   The `decls` span holds all top‑level declarations in source order.
 *   Each declaration is an ASTPtr<DeclAST> (unique_ptr with no‑op deleter).
 *   The underlying memory is arena‑allocated; the unique_ptr is just an
 *   ownership wrapper that does not call delete.
 *
 * @note Relationship with PackageDeclAST
 *   ProgramAST duplicates the package name as a convenience. The separate
 *   PackageDeclAST node exists primarily for source location tracking when
 *   the package name mismatches the directory structure.
 *
 * @field packageName The package name declared by `package foo` at file start.
 * @field filePath    Relative path from package root (e.g., "math/vec2.luc").
 * @field decls       Top‑level declarations in source order.
 */
struct ProgramAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    InternedString     packageName;   // from `package foo`
    InternedString     filePath;      // relative path (e.g., "math/vec2.luc")
    ArenaSpan<DeclPtr> decls;         // top‑level declarations in source order

    ProgramAST() : BaseAST(ASTKind::Program) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamAST – a generic type parameter (e.g., `<T : Drawable>`).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a single generic type parameter declaration.
 *
 * This node appears in the generic parameter list of functions, structs,
 * traits, impl blocks, and type aliases. Each parameter has a name and
 * an optional list of trait constraints.
 *
 * @par Grammar Reference (from LUC_GRAMMAR.md)
 *   generic_param := IDENTIFIER
 *                  | IDENTIFIER ':' IDENTIFIER
 *                  | IDENTIFIER ':' constraint_list
 *   constraint_list := IDENTIFIER { '+' IDENTIFIER }
 *
 * @par Examples
 *   @code
 *   struct Box<T> { ... }                    // unconstrained T
 *   fn find<T : Comparable> (items []T) ...  // T must implement Comparable
 *   type Pair<K, V> = struct { ... }         // two unconstrained parameters
 *   impl Map<K : Hashable, V> { ... }        // K constrained, V unconstrained
 *   @endcode
 *
 * @par Memory Layout (64-bit, typical)
 *   - BaseAST overhead    : ~16 bytes (vtable + kind + loc + padding)
 *   - `name`              : 4 bytes (InternedString is uint32_t)
 *   - `constraints` span  : 16 bytes (ptr + size, each 8 bytes)
 *   @n Total: ~36 bytes per generic parameter (excluding constraint strings)
 *
 * @par Semantic Resolution
 *   During semantic analysis, each constraint name is resolved to a
 *   `TraitDeclAST`. The order of constraints does not affect semantics,
 *   but is preserved for source fidelity.
 *
 * @field name        The identifier of the type parameter (e.g., "T", "K", "V").
 * @field constraints Trait names that this parameter must satisfy.
 *                    Empty span means the parameter is unconstrained.
 *
 * @note Multiple constraints are joined with `+` in source (e.g., `T : Hashable + Comparable`).
 *       The semantic pass verifies that all constraint names resolve to traits
 *       and that the traits are compatible (no conflicting method signatures).
 */
struct GenericParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParam;

    InternedString name;
    ArenaSpan<InternedString> constraints;   // trait names (empty = unconstrained)

    explicit GenericParamAST(InternedString n)
        : BaseAST(ASTKind::GenericParam), name(n) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// ParamAST – a function parameter (name, type, variadic flag).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a single parameter in a function or method signature.
 *
 * This node appears in the parameter list of `FuncDeclAST`, `MethodDeclAST`,
 * `TraitMethodAST`, `FromEntryAST`, and `AnonFuncExprAST`. Each parameter
 * has a name and an explicit type annotation (Luc requires explicit types
 * everywhere – no type inference).
 *
 * @par Grammar Reference (from LUC_GRAMMAR.md)
 *   param := IDENTIFIER type
 *   variadic_param := IDENTIFIER '...' type
 *
 * @par Examples
 *   @code
 *   let add (a int, b int) -> int           // two normal parameters
 *   let printf (fmt string, args ...any)    // variadic parameter
 *   fn map<T, U> (items []T, f (T) -> U)    // function parameter
 *   @endcode
 *
 * @par Memory Layout (64-bit, typical)
 *   - BaseAST overhead    : ~16 bytes (vtable + kind + loc + padding)
 *   - `name`              : 4 bytes (InternedString)
 *   - `type` (unique_ptr) : 8 bytes (pointer to arena‑allocated TypeAST)
 *   - `isVariadic`        : 1 byte
 *   - padding             : 7 bytes (alignment to 8 bytes)
 *   @n Total: ~36 bytes per parameter (excluding the TypeAST node itself)
 *
 * @par Semantic Rules
 *   - Only the **last** parameter in a parameter group may be variadic.
 *   - Variadic parameters are only allowed in `@extern` functions and
 *     in standard library variadic functions (e.g., `printf`-style).
 *   - The type of a variadic parameter must be `any` (for `@extern` C variadics)
 *     or a concrete array type for Luc‑side variadics.
 *
 * @field name        Parameter name (e.g., "x", "items", "args").
 * @field type        Explicit type annotation (never null).
 * @field isVariadic  True if this is the variadic `...` parameter.
 *
 * @see FuncSignature for how parameters are grouped into curry groups.
 */
struct ParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    InternedString name;
    TypePtr        type;
    bool           isVariadic = false;

    // semantic
    bool isConst = false; // true for compile‑time constants

    ParamAST() : BaseAST(ASTKind::Param) {}
};

using ParamPtr          = ASTPtr<ParamAST>;
using ParamGroup        = std::vector<ParamPtr>;
using GenericParamPtr   = ASTPtr<GenericParamAST>;

// ─────────────────────────────────────────────────────────────────────────────
// UnknownAST family – error recovery nodes.
//
// These nodes are produced by the parser when it encounters syntax errors
// but can continue parsing. They act as placeholders, allowing the AST to
// remain well‑formed even when parts of the source code are invalid.
//
// ## When are these created?
//   - Unexpected token where an expression was expected → UnknownExprAST
//   - Invalid declaration syntax → UnknownDeclAST
//   - Malformed statement → UnknownStmtAST
//   - Unrecognised type annotation → UnknownTypeAST
//   - Generic fallback when the exact kind is unknown → UnknownAST
//
// ## How are they used?
//   1. The parser creates one via `arena.make<UnknownExprAST>()`
//   2. The node is inserted into the AST at the error location
//   3. Semantic passes may skip or ignore unknown nodes
//   4. Code generation should never receive unknown nodes (semantic pass
//      should abort compilation if any remain after error recovery)
//
// ## Why have multiple unknown kinds?
//   - `UnknownDeclAST` : can be stored in `ArenaSpan<DeclPtr>`
//   - `UnknownExprAST` : can be stored in `ArenaSpan<ExprPtr>`
//   - `UnknownStmtAST` : can be stored in `ArenaSpan<StmtPtr>`
//   - `UnknownTypeAST` : can be stored in `ArenaSpan<TypePtr>`
//   - `UnknownAST`     : generic fallback when the context is ambiguous
//
//   Using the correct subtype preserves type safety in collections
//   while still indicating an error occurred.
//
// ## Memory Layout
//   All unknown nodes are trivial – they add no fields beyond BaseAST.
//   Size: ~16 bytes (vtable + kind + loc + padding)
//
// @see isUnknown() helper for checking any unknown node kind
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Generic unknown node – fallback when the specific kind is ambiguous.
 *
 * Used only when the parser cannot determine whether the invalid syntax
 * was a declaration, expression, statement, or type. Prefer the more
 * specific unknown node types when possible.
 */
struct UnknownAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Unknown;

    UnknownAST() : BaseAST(ASTKind::Unknown) {}
};

/**
 * @brief Helper to check if a node is any kind of unknown node.
 *
 * Returns true for:
 *   - UnknownAST (generic)
 *   - UnknownDeclAST
 *   - UnknownExprAST
 *   - UnknownStmtAST
 *   - UnknownTypeAST
 *
 * Also returns true for `nullptr` (treats missing node as unknown).
 *
 * @param node The AST node to check (may be null).
 * @return true if the node is unknown, null, or an error recovery node.
 *
 * @par Usage
 *   @code
 *   if (isUnknown(node)) {
 *       // Skip this node during semantic analysis
 *       return;
 *   }
 *   @endcode
 */
inline bool isUnknown(const BaseAST* node) {
    if (!node) return true;
    switch (node->kind) {
        case ASTKind::Unknown:
        case ASTKind::UnknownDecl:
        case ASTKind::UnknownExpr:
        case ASTKind::UnknownStmt:
        case ASTKind::UnknownType:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Unknown declaration – produced when a declaration is malformed.
 *
 * Examples:
 *   - Missing identifier: `let = 5`
 *   - Invalid visibility: `pub pub struct X`
 *   - Malformed generic parameters: `struct Box<T :`
 *
 * This node can appear in `ArenaSpan<DeclPtr>` (e.g., `ProgramAST::decls`).
 * Semantic analysis should skip it and report the original parse error.
 */
struct UnknownDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownDecl;
    UnknownDeclAST() : DeclAST(ASTKind::UnknownDecl) {}
};

/**
 * @brief Unknown expression – produced when an expression is malformed.
 *
 * Examples:
 *   - Unmatched parentheses: `(1 + 2`
 *   - Missing operand: `x +`
 *   - Invalid operator sequence: `x * / y`
 *
 * This node can appear in `ArenaSpan<ExprPtr>` (e.g., function arguments,
 * initialisers, condition expressions). Semantic analysis should skip it.
 */
struct UnknownExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownExpr;
    UnknownExprAST() : ExprAST(ASTKind::UnknownExpr) {}
};

/**
 * @brief Unknown statement – produced when a statement is malformed.
 *
 * Examples:
 *   - Incomplete `if`: `if x {` (missing closing brace)
 *   - Missing loop body: `for i in 0..10`
 *   - Invalid `return` placement: `return break`
 *
 * This node can appear in `ArenaSpan<StmtPtr>` (e.g., block bodies,
 * loop bodies, if branches). Semantic analysis should skip it.
 */
struct UnknownStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownStmt;
    UnknownStmtAST() : StmtAST(ASTKind::UnknownStmt) {}
};

/**
 * @brief Unknown type – produced when a type annotation is malformed.
 *
 * Examples:
 *   - Missing bracket: `[_, int`
 *   - Invalid array syntax: `[10` 
 *   - Unknown primitive: `int128`
 *   - Unterminated generic: `Vec2<int`
 *
 * This node can appear in `ArenaSpan<TypePtr>` (e.g., variable types,
 * function return types, generic arguments). Semantic analysis should
 * treat it as an unresolved type and propagate the error.
 */
struct UnknownTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownType;
    UnknownTypeAST() : TypeAST(ASTKind::UnknownType) {}
};