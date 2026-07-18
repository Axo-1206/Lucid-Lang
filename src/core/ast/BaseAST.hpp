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
#include "../SourceLocation.hpp"
#include "../memory//ASTArena.hpp"
#include "../memory/InternedString.hpp"
#include "../memory//ArenaSpan.hpp"

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
//   DeclAST.hpp     — FuncDeclAST, StructDeclAST, TraitDeclAST, ...
//   ExprAST.hpp     — LiteralExprAST, CallExprAST, PipelineExprAST, ...
//   StmtAST.hpp     — BlockStmtAST, ForStmtAST, ...
// ─────────────────────────────────────────────────────────────────────────────

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct ArrayTypeAST;
struct NullableTypeAST;
struct FallibleTypeAST;
struct CombinedTypeAST;
struct RefTypeAST;
struct PtrTypeAST;
struct FuncTypeAST;
struct TupleTypeAST;

// DeclAST.hpp
struct ImportDeclAST;
struct VarDeclAST;
struct ParamAST;
struct GenericParamDeclAST;
struct FuncDeclAST;
struct FieldDeclAST;
struct StructDeclAST;
struct EnumVariantAST;
struct EnumDeclAST;
struct TraitFieldDeclAST;
struct TraitDeclAST;
struct TraitRefAST;

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
struct ModuleAccessExprAST;
struct NullableChainExprAST;
struct NullCoalesceExprAST;
struct AssignExprAST;
struct PipelineExprAST;
struct PipelineStepAST;
struct ComposeExprAST;
struct ComposeOperandAST;
struct AnonFuncExprAST;
struct IfExprAST;
struct RangeExprAST;

// Concurrency
struct AsyncStmtAST;
struct AwaitStmtAST;
struct SpawnStmtAST;
struct JoinStmtAST;

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
struct ModuleAST;

// Special
struct ValueDeclAST;
struct TypeDeclAST;

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
    
    // Special
    ValueDecl,
    TypeDecl,

    // Type nodes
    PrimitiveType,
    NamedType,
    ArrayType,
    NullableType,
    FallibleType,
    CombinedType,      // T?!
    RefType,
    PtrType,
    FuncType,
    TupleType,

    // Declaration nodes
    ImportDecl,
    VarDecl,
    Param,
    GenericParamDecl,
    FuncDecl,
    FieldDecl,
    StructDecl,
    EnumVariant,
    EnumDecl,
    TraitFieldDecl,
    TraitDecl,
    TraitRef,

    // Expression nodes
    LiteralExpr,
    ArrayLiteralExpr,
    StructLiteralExpr,
    FieldInit,
    IdentifierExpr,
    FieldAccessExpr,
    ModuleAccessExpr,
    CallExpr,
    IndexExpr,
    SliceExpr,
    BinaryExpr,
    UnaryExpr,
    AssignExpr,
    NullableChainExpr,
    NullCoalesceExpr,
    PipelineExpr,
    PipelineStep,
    ComposeExpr,
    ComposeOperand,
    AnonFuncExpr,
    IfExpr,
    RangeExpr,
    TupleExpr,

    // Concurrency
    AsyncExpr,
    AwaitExpr,
    SpawnExpr,
    JoinExpr,

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

    // Root
    Program,

    // Compiler directives
    Attribute,
    AttributeArg,
    IntrinsicCallExpr,
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
    FloatLit,    // 3.14
    BoolLit,     // true, false
    TypeIdent    // TypeName (e.g., @foreign("C"))
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

struct TypeAST : BaseAST {
    explicit TypeAST(ASTKind k) : BaseAST(k) {}
};

struct ExprAST : BaseAST {
    TypeAST* resolvedType = nullptr;
    bool isModuleMember   = false;
    bool isConst          = false;

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};

struct StmtAST : BaseAST {
    explicit StmtAST(ASTKind k) : BaseAST(k) {}
};

struct DeclAST : BaseAST {
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
    static constexpr ASTKind staticKind = ASTKind::ValueDecl;

    // Cached resolved type of this value (set during type resolution)
    // For functions, this points to funcType
    TypeAST* valueType = nullptr;
    InternedString name;
    
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
 *
 * ─── Self‑Type Cache ────────────────────────────────────────────────────────
 * The `selfType` field caches a NamedTypeAST that represents this type itself.
 * This is used when a type name appears as a value (e.g., `int("42")` where `int`
 * is used as a conversion function). Without this cache, we would need to
 * create a new NamedTypeAST every time a type name is referenced.
 *
 * @note TypeDeclAST nodes are stored in Scope::types map.
 */
struct TypeDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeDecl;

    // Self-type reference (e.g., "Point" as a NamedTypeAST)
    // Used when the type name appears as a value (e.g., `int("42")`)
    // Mutable because it's set lazily during semantic analysis
    mutable NamedTypeAST* selfType = nullptr;

    InternedString name;
    
    explicit TypeDeclAST(ASTKind k) : DeclAST(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Ownership aliases — all pointers are raw because the arena owns all memory.
// ─────────────────────────────────────────────────────────────────────────────

using TypePtr    = TypeAST*;
using DeclPtr    = DeclAST*;
using ExprPtr    = ExprAST*;
using StmtPtr    = StmtAST*;

// ─────────────────────────────────────────────────────────────────────────────
// ModuleAST — root node for a single translation unit.
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
 * @field packageName The package name declared by `package foo` at file start.
 * @field filePath    Relative path from package root (e.g., "math/vec2.luc").
 * @field decls       Top‑level declarations in source order.
 *
 * @note Diagnostics for this module are NOT stored here. They live in the
 *       `diagnostic` namespace's own whole-session list (see
 *       Diagnostic.hpp), keyed by `filePath` — get them with
 *       `diagnostic::getAllForFile(module->filePath)`. Storing a second
 *       copy on the node itself was removed once the diagnostic system
 *       started tracking file association on its own (see
 *       `diagnostic::pushSource()`/`getAllForFile()`); keeping one here
 *       too would just be the same data living in two places again, the
 *       exact duplication the diagnostic-system rewrite was meant to
 *       eliminate.
 *
 *       `hasErrors` remains as a cheap cached bool — a single flag, set
 *       once from `diagnostic::hasErrorsInCurrentSource()` right after
 *       this module finishes parsing/analysis, so callers that only need
 *       "did this succeed" don't have to make a lookup (or pull in
 *       Diagnostic.hpp at all) just to check a yes/no. That's a small
 *       derived snapshot, not a duplicate store, which is why it stayed
 *       while `errors` didn't.
 */
struct ModuleAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    InternedString       filePath;
    ArenaSpan<DeclPtr>   decls;
    bool hasErrors = false;

    ModuleAST() : BaseAST(ASTKind::Program) {}
};
using ModuleASTPtr = ModuleAST*;

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamDeclAST — a generic type parameter declaration.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a single generic type parameter declaration.
 *
 * This node appears in the generic parameter list of functions, structs,
 * and traits. Each parameter has a name and an optional list of trait constraints.
 *
 * @par Grammar Reference (from LUCID_GRAMMAR.md)
 *   generic_param := IDENTIFIER
 *                  | IDENTIFIER ':' trait_ref { '+' trait_ref }
 *
 * @par Examples
 *   @code
 *   struct Box<T> { ... }                         // unconstrained T
 *   const magnitude<T : Vector2> (v T) -> float   // T must implement Vector2
 *   struct Pair<A : Named, B : Named> { ... }     // two constrained parameters
 *   @endcode
 *
 * @par Memory Layout (64-bit, typical)
 *   - BaseAST overhead    : ~16 bytes (vtable + kind + loc + padding)
 *   - `name`              : 4 bytes (InternedString is uint32_t)
 *   - `constraints` span  : 16 bytes (ptr + size, each 8 bytes)
 *   @n Total: ~36 bytes per generic parameter (excluding constraint nodes)
 *
 * @par Semantic Resolution
 *   During semantic analysis, each constraint type is resolved to a
 *   `TraitDeclAST`. The order of constraints does not affect semantics,
 *   but is preserved for source fidelity.
 *
 * @field name        The identifier of the type parameter (e.g., "T", "K", "V").
 * @field constraints Trait types that this parameter must satisfy.
 *                    Empty span means the parameter is unconstrained.
 *                    Each constraint is a TraitRefAST node.
 *
 * @note Multiple constraints are joined with `+` in source (e.g., `T : Vector2 + Named`).
 *       The semantic pass verifies that all constraint types resolve to traits
 *       and that the traits are compatible.
 */
struct GenericParamDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParamDecl;

    InternedString name;
    ArenaSpan<TraitRefAST*> constraints;   // trait refs (empty = unconstrained)

    explicit GenericParamDeclAST(InternedString n)
        : BaseAST(ASTKind::GenericParamDecl), name(n) {}
};

using ParamPtr          = ParamAST*;
using ParamGroup        = std::vector<ParamPtr>;
using GenericParamDeclPtr   = GenericParamDeclAST*;

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