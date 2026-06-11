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
//   TypeAST.hpp     — PrimitiveTypeAST, NamedTypeAST, GenericParamTypeAST, ...
//   DeclAST.hpp     — FuncDeclAST, StructDeclAST, ImplDeclAST, ...
//   ExprAST.hpp     — LiteralExprAST, CallExprAST, PipelineExprAST, ...
//   StmtAST.hpp     — BlockStmtAST, ForStmtAST, ...
// ─────────────────────────────────────────────────────────────────────────────

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct ArrayTypeAST;
struct GenericParamTypeAST;
struct NullableTypeAST;
struct ResultTypeAST;        // T!E / T! — result type;
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
struct SliceExprAST;
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
    ArrayType,
    GenericParamType,
    NullableType,
    ResultType,
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
    LiteralExpr,
    ArrayLiteralExpr,
    StructLiteralExpr,
    FieldInit,
    IdentifierExpr,
    FieldAccessExpr,
    BehaviorAccessExpr,
    CallExpr,
    IndexExpr,
    SliceExpr,
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
    ResolveExpr,
    OkArm,
    ErrArm,
    MatchExpr,
    IfExpr,
    RangeExpr,

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
// TargetKind — distinguishes the kind of target in impl and from declarations
// ─────────────────────────────────────────────────────────────────────────────

enum class TargetKind {
    Concrete,           // int, Vec2, [_, int] — no free type variables
    GenericArray,       // [_, <T>], [*, <T>], [N, <T>]
    GenericNamed        // Box<T> — struct or type alias with generic params
};

// ─────────────────────────────────────────────────────────────────────────────
// FromEntryKind — distinguishes inline entry from path entry in from blocks
// ─────────────────────────────────────────────────────────────────────────────

enum class FromEntryKind {
    Inline,     // param_group ... -> type = func_body
    Path        // func_ref
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

struct BaseAST {
    ASTKind kind;
    SourceLocation loc;

    explicit BaseAST(ASTKind k) : kind(k) {}
    virtual ~BaseAST() = default;

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
    InternedString   value;

    AttributeArgAST(AttributeArgKind k, InternedString v)
        : BaseAST(ASTKind::AttributeArg), kind(k), value(v) {}
};

using AttributeArgPtr = AttributeArgAST*;

struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    InternedString name;
    ArenaSpan<AttributeArgPtr> args;

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
};

using AttributePtr = AttributeAST*;

// ─────────────────────────────────────────────────────────────────────────────
// Family bases
// ─────────────────────────────────────────────────────────────────────────────

struct TypeAST    : BaseAST { explicit TypeAST(ASTKind k)    : BaseAST(k) {} };
struct ExprAST    : BaseAST {
    TypeAST* resolvedType = nullptr;
    bool isBehaviorMember = false;
    bool isConst          = false;

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};
struct StmtAST    : BaseAST { explicit StmtAST(ASTKind k) : BaseAST(k) {} };
struct PatternAST : BaseAST {
    TypeAST* resolvedType = nullptr;
    explicit PatternAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};

struct DeclAST    : BaseAST {
    std::optional<DocComment> doc;
    ArenaSpan<AttributePtr>   attributes;
    InternedString            file;
    bool                      isConst = false;

    explicit DeclAST(ASTKind k) : BaseAST(k) {}
    bool hasDoc() const { return doc.has_value(); }
};
// ─────────────────────────────────────────────────────────────────────────────
// ValueDeclAST – base for declarations that produce values
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Base class for declarations that produce values (can appear in expressions).
 *
 * Value declarations live in the VALUE NAMESPACE. When an identifier is resolved
 * in an expression context, the lookup searches this namespace first.
 *
 * Value declarations include:
 *   - Variables (VarDeclAST)
 *   - Functions (FuncDeclAST)
 *   - Parameters (ParamAST)
 *   - Fields (FieldDeclAST)
 *   - Methods (MethodDeclAST)
 *   - Enum variants (EnumVariantAST)
 *
 * ─── Type Cache ─────────────────────────────────────────────────────────────
 * The `valueType` field caches the resolved type of this value. For example:
 *   - For a variable: its declared type
 *   - For a function: its function type (FuncTypeAST)
 *   - For a parameter: its parameter type
 *
 * This eliminates the need for a separate symbol table entry.
 *
 * @note ValueDeclAST nodes are stored in Scope::values map.
 */
struct ValueDeclAST : DeclAST {
    InternedString name;
    // Cached resolved type of this value (set during type resolution)
    // For functions, this points to funcType
    TypeAST* valueType = nullptr;
    
    explicit ValueDeclAST(ASTKind k) : DeclAST(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeDeclAST – base for declarations that define types
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Base class for declarations that define types.
 *
 * Type declarations live in the TYPE NAMESPACE. When an identifier is resolved
 * in a type annotation context, the lookup searches this namespace.
 *
 * Type declarations include:
 *   - Structs (StructDeclAST)
 *   - Enums (EnumDeclAST)
 *   - Traits (TraitDeclAST)
 *   - Type aliases (TypeAliasDeclAST)
 *
 * ─── Self‑Type Cache ────────────────────────────────────────────────────────
 * The `selfType` field caches a NamedTypeAST that represents this type itself.
 * This is used when a type name appears as a value (e.g., `int("42")` where `int`
 * is used as a conversion function). Without this cache, we would need to
 * create a new NamedTypeAST every time a type name is referenced.
 *
 * ─── Alias Resolution ───────────────────────────────────────────────────────
 * For type aliases, `resolvedType` stores the underlying type after unwrapping
 * all alias chains (e.g., `type A = B; type B = int` → `resolvedType = int`).
 * This provides O(1) access to the ultimate type without repeated lookups.
 *
 * @note TypeDeclAST nodes are stored in Scope::types map.
 */
struct TypeDeclAST : DeclAST {
    InternedString name;
    // Self-type reference (e.g., "Point" as a NamedTypeAST)
    // Used when the type name appears as a value (e.g., `int("42")`)
    // Mutable because it's set lazily during semantic analysis
    mutable NamedTypeAST* selfType = nullptr;
    
    // For type aliases: resolved underlying type (after unwrapping chains)
    // For non-aliases, this may be nullptr or points to selfType
    TypeAST* resolvedType = nullptr;
    
    explicit TypeDeclAST(ASTKind k) : DeclAST(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Ownership aliases — all pointers are raw because the arena owns all memory.
// ─────────────────────────────────────────────────────────────────────────────

using TypePtr    = TypeAST*;
using DeclPtr    = DeclAST*;
using ExprPtr    = ExprAST*;
using StmtPtr    = StmtAST*;
using PatternPtr = PatternAST*;

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

    InternedString     packageName;
    InternedString     filePath;
    ArenaSpan<DeclPtr> decls;

    ProgramAST() : BaseAST(ASTKind::Program) {}
};
using ProgramASTPtr = ProgramAST*;

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamAST — a generic type parameter declaration.
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

using ParamPtr          = ParamAST*;
using ParamGroup        = std::vector<ParamPtr>;
using GenericParamPtr   = GenericParamAST*;

// ─────────────────────────────────────────────────────────────────────────────
// UnknownAST family — error recovery nodes.
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

struct UnknownDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownDecl;
    UnknownDeclAST() : DeclAST(ASTKind::UnknownDecl) {}
};

struct UnknownExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownExpr;
    UnknownExprAST() : ExprAST(ASTKind::UnknownExpr) {}
};

struct UnknownStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownStmt;
    UnknownStmtAST() : StmtAST(ASTKind::UnknownStmt) {}
};

struct UnknownTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownType;
    UnknownTypeAST() : TypeAST(ASTKind::UnknownType) {}
};