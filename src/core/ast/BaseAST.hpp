/// @file BaseAST.hpp
/// 
/// @responsibility The Foundation. Defines the BaseAST, the Visitor interface,
///                 and common types (DocComment, SourceLocation, ASTKind).
/// 
/// @architectural_note
///   This file uses Forward Declarations for all AST families (Expr, Stmt, etc.).
///   NEVER include a family header (like ExprAST.hpp) here; this keeps the
///   dependency graph acyclic.
/// 
/// @related_files
///   - src/ast/ExprAST.hpp, StmtAST.hpp, DeclAST.hpp, TypeAST.hpp
///   - Each family header includes BaseAST.hpp, not the other way around.
/// 
/// ============================================================================
/// FIELD CATEGORIES
/// ============================================================================
/// 
/// AST fields are organized into four categories based on who sets them and when:
/// 
/// | Category        | Mutability          | Set By  | Examples                                    |
/// | --------------- | ------------------- | ------- | ------------------------------------------- |
/// | Parser Fields   | `const` (immutable) | Parser  | `name`, `type`, `init`, `body`              |
/// | Semantic Fields | `mutable`           | Sema    | `resolvedType`, `constValue`, `isLValue`    |
/// | Layout Fields   | `mutable`           | Sema    | `fieldIndex`, `byteOffset`, `totalSize`     |
/// | CodeGen Fields  | `mutable`           | CodeGen | `llvmValue`, `llvmFunction`, `llvmAlloca`   |
/// 
/// ## Layout Fields vs CodeGen Fields
/// 
/// Layout fields (fieldIndex, byteOffset, totalSize, alignment) are computed
/// by Sema during semantic analysis. They represent decisions about the
/// memory layout of types, which are independent of the target machine.
/// 
/// CodeGen fields (llvmType, llvmFunction, llvmAlloca) are created during
/// IR lowering. They are actual LLVM IR objects that don't exist until
/// CodeGen runs.
/// 
/// This separation allows:
///   1. Sema to validate layout decisions (e.g., no self-referential structs)
///   2. CodeGen to focus on IR generation without recomputation
///   3. Clear ownership of each field's lifecycle

#pragma once

#include "debug/DebugMacros.hpp"
#include "../SourceLocation.hpp"
#include "../memory/ASTArena.hpp"
#include "../memory/InternedString.hpp"
#include "../memory/ArenaSpan.hpp"

#include <string>
#include <optional>
#include <memory>
#include <variant>
#include <vector>
#include <unordered_map>
#include <cassert>

// ─── LLVM Headers ──────────────────────────────────────────────────────────
// These are needed for CodeGen annotation fields. Parser and Sema don't use
// these fields, but including the headers is fine.
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>

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
struct ModuleTypeAccessAST;
struct ArrayTypeAST;
struct NullableTypeAST;
struct FallibleTypeAST;
struct CombinedTypeAST;
struct RefTypeAST;
struct PtrTypeAST;
struct FuncTypeAST;

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
struct FuncRefStmtAST;

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
    ModuleTypeAccess,
    ArrayType,
    NullableType,
    FallibleType,
    CombinedType,      // T?!
    RefType,
    PtrType,
    FuncType,
    FutureType,        // Future<T> — result of `async`, consumed exactly once by `await`
    ThreadType,        // Thread<T> — result of `spawn`, consumed exactly once by `join`

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

    // Concurrency
    AsyncStmt,
    AwaitStmt,
    SpawnStmt,
    JoinStmt,

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
    FuncRefStmt,

    // Root
    Program,

    // Compiler directives
    Attribute,
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

    // ─── Type Checking ──────────────────────────────────────────────────────

    template<typename T>
    bool isa() const { return kind == T::staticKind; }

    template<typename T>
    T* as() {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>()");
        return static_cast<T*>(this);
    }

    template<typename T>
    T* as() const {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>()");
        return static_cast<const T*>(this);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Family bases
// ─────────────────────────────────────────────────────────────────────────────

struct StmtAST : BaseAST {
    explicit StmtAST(ASTKind k) : BaseAST(k) {}
};

struct DeclAST : BaseAST {
    std::optional<DocComment> doc;
    ArenaSpan<AttributeAST*>  attributes;
    const InternedString      name;

    explicit DeclAST(ASTKind k, InternedString n) : BaseAST(k), name(n) {}
    bool hasDoc() const { return doc.has_value(); }
};

struct TypeAST : BaseAST {
    explicit TypeAST(ASTKind k) : BaseAST(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONSTANT EVALUATION DESIGN
// ─────────────────────────────────────────────────────────────────────────────
//
// Lucid evaluates constant expressions (e.g., `2 + 3`, `if true ?? 5 else 10`)
// during semantic analysis. The result is stored as metadata on the original
// AST node, not by replacing the expression subtree with a literal.
//
// ## Why Metadata, Not Replacement?
//
// 1. **Memory efficiency**: Replacing AST nodes during semantic analysis would
//    require allocating new nodes (via the arena) for every constant expression.
//    The original AST is already allocated; reusing it avoids extra memory
//    pressure and fragmentation.
//
// 2. **Source fidelity**: Preserving the original AST is essential for
//    diagnostics. When an error occurs, we can report it in terms of the
//    original source expression, not a transformed one.
//
// 3. **Non‑destructive analysis**: Other semantic passes may need to traverse
//    the original expression tree (e.g., for type checking, narrowing, or
//    capture analysis). Replacing the AST would break these passes.
//
// 4. **Lazy evaluation**: We can compute and cache the constant value once,
//    and reuse it wherever needed, without modifying the AST.
//
// ## Implementation Fields (on ExprAST)
//
//   - `isConst` : bool
//         True if the expression has been evaluated to a compile‑time constant.
//         Set by `ConstEvaluator`; never changes after that.
//
//   - `constValue` : ConstantValue
//         The evaluated constant value (if `isConst` is true). May be a
//         primitive, enum, struct, array, or function pointer.
//
//   - `valueState` : ValueState
//         Reflects the result's nullability/fallibility state (Definite, Nil,
//         Err, Unknown, None). Helps with flow‑sensitive narrowing.
//
//   - `resolvedType` : TypeAST*
//         The semantic type of the expression, set during type resolution.
//         For constants, this is the type of the evaluated value.
//
// ## Usage Guidelines
//
// ### Semantic Analysis (Sema)
//   - Call `ConstEvaluator::evaluate(ctx, expr, targetType)` to evaluate an
//     expression. It returns a `ConstantValue` and sets `isConst`/`constValue`
//     on the node if successful.
//   - Use `expr->isConst` to check if a constant is available.
//   - Access the evaluated value via `expr->constValue`.
//   - Do not modify the AST structure; use the metadata fields.
//
// ### Code Generation (CodeGen)
//   - If `expr->isConst` is true, you may emit the constant directly
//     (e.g., `emitConstant(expr->constValue)`).
//   - Otherwise, emit the expression as usual.
//
// ### Diagnostics
//   - When reporting an error, refer to the original expression's source
//     location (`expr->loc`) and, if helpful, include the evaluated constant
//     value in the message.
//
// ## Important Note
//
// The const evaluator never replaces the original AST node with a literal.
// The original structure remains intact for diagnostics and other passes.
// Metadata fields are the only addition.
//
// ─────────────────────────────────────────────────────────────────────────────

enum class ValueState {
    None,       // For any call expression that return no value
    Definite,   // Produces a definite value (T)
    Nil,        // Produces nil (T?)
    Err,        // Produces err (T!)
    Unknown,    // Unknown at compile-time (needs runtime evaluation)
};

// ─────────────────────────────────────────────────────────────────────────────
// CapturedVariable — Information about a variable captured by a closure.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a variable captured by a closure.
/// 
/// This struct is populated by Sema during capture analysis and used by
/// CodeGen to generate the closure environment.
/// 
/// ─── Why This Is a Distinct Struct, Not `ArenaSpan<ValueDeclAST*>` ─────────
/// Every field below (`byReference`, `index`, `envSlot`) is a property of the
/// *(this closure, this declaration)* pair, not of the declaration alone —
/// the same `ValueDeclAST` may be captured mutably by one closure and
/// read-only by another, and will generally have a different `index`/
/// `envSlot` in each closure's own environment struct. None of this can be
/// hoisted onto `ValueDeclAST` itself without either storing a list keyed by
/// capturing closure on every declaration (worse: declarations vastly
/// outnumber closures, and most are never captured by anything) or losing
/// the information outright.
/// 
/// @field decl          The declaration of the captured variable.
/// @field byReference   True if this closure may write to the captured
///                      variable, and therefore must share one heap slot
///                      with every other holder (the enclosing frame, and
///                      any other closure capturing the same declaration).
///                      False if this closure only reads it, in which case
///                      it may instead be snapshot-copied into the
///                      environment at construction time — see the Capture
///                      Rules note on `AnonFuncExprAST` for when that
///                      optimization is safe.
/// @field index         Index in the closure environment (set by Sema).
struct CapturedVariable {
    ValueDeclAST* decl = nullptr;
    bool byReference = false;
    size_t index = 0;
    
    // ─── CodeGen Annotations ──────────────────────────────────────────
    llvm::Value* envSlot = nullptr;   // LLVM slot in the environment
};

// ─────────────────────────────────────────────────────────────────────────────
// ExprAST — Base class for all expression nodes.
// ─────────────────────────────────────────────────────────────────────────────

struct ExprAST : BaseAST {
    // ─── Parser Fields ──────────────────────────────────────────────────
    
    // ─── Semantic Fields (set by Sema) ────────────────────────────────
    TypeAST* resolvedType = nullptr;  // The resolved type of this expression
    ValueState valueState = ValueState::Unknown;  // Nil/Err/Definite/Unknown
    bool isLValue = false;                  // Can this appear on LHS of assignment?
    bool isConst = false;                   // Is this a compile-time constant?
    
    // ─── CodeGen Fields (set by CodeGen) ──────────────────────────────
    llvm::Value* llvmValue = nullptr;       // The generated LLVM value

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
    
    // Convenience methods
    bool isNone() const { return valueState == ValueState::None; }
    bool isDefinite() const { return valueState == ValueState::Definite; }
    bool isNil() const { return valueState == ValueState::Nil; }
    bool isErr() const { return valueState == ValueState::Err; }
    bool isUnknown() const { return valueState == ValueState::Unknown; }
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeAST — represents an attribute attached to a declaration.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents an attribute attached to a declaration.
///
/// Attributes are compiler directives that provide additional information
/// to the compiler. Each attribute has a name and an optional list of
/// literal arguments.
///
/// @note Arguments are restricted to literals only (no expressions).
///       The parser enforces this restriction by parsing LiteralExprAST.
///
/// @example
///   @[export]                      → name="export", args={}
///   @[foreign("C")]                → name="foreign", args=[String("C")]
///   @[deprecated("use new")]       → name="deprecated", args=[String("use new")]
///   @[link("opengl", "m")]         → name="link", args=[String("opengl"), String("m")]
struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    InternedString name;
    ArenaSpan<LiteralExprAST*> args;

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
};
using AttributePtr = AttributeAST*;

// ─────────────────────────────────────────────────────────────────────────────
// ValueDeclAST – base for declarations that produce values
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Distinguishes between mutable and immutable declarations.
/// 
/// - Let:   mutable binding (can be reassigned)
/// - Const: immutable binding (cannot be reassigned)
/// 
/// @note For struct fields, `Const` means the field cannot be reassigned
///       after construction, even if the containing variable is `let`.
enum class DeclKeyword {
    Let,    // mutable
    Const   // immutable
};

/// @brief Base class for declarations that produce values (can appear in expressions).
/// 
/// Value declarations live in the VALUE NAMESPACE. When an identifier is resolved
/// in an expression context, the lookup searches this namespace first.
/// 
/// Value declarations include:
///   - Variables (VarDeclAST)
///   - Functions (FuncDeclAST)
///   - Parameters (ParamAST)
///   - Fields (FieldDeclAST)
///   - Enum variants (EnumVariantAST)
/// 
/// ─── Const-ness ─────────────────────────────────────────────────────────────
/// The `keyword` field determines whether this value can be mutated:
///   - `DeclKeyword::Let`:  mutable (can be reassigned)
///   - `DeclKeyword::Const`: immutable (cannot be reassigned)
/// 
/// For enum variants, the keyword is always `Const` (they are immutable constants).
/// 
/// ─── Type Resolution ─────────────────────────────────────────────────────────
/// The `resolvedType` field stores the fully resolved type of this declaration.
/// This is set during semantic analysis (Phase 2) and is used by expression
/// resolvers when an identifier references this declaration.
/// 
/// @note ValueDeclAST nodes are stored in Scope::values map.
struct ValueDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ValueDecl;

    /// The keyword that determines mutability (Let = mutable, Const = immutable)
    const DeclKeyword keyword;
    TypeAST* type = nullptr;
    
    /// @brief Check if this value is immutable (const).
    bool isConst() const { return keyword == DeclKeyword::Const; }
    
    /// @brief Check if this value is mutable (let).
    bool isLet() const { return keyword == DeclKeyword::Let; }
    
    explicit ValueDeclAST(ASTKind k, InternedString n, DeclKeyword kw, TypeAST* t)
        : DeclAST(k, n), keyword(kw), type(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeDeclAST – base for declarations that define types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base class for declarations that define types.
/// 
/// Type declarations live in the TYPE NAMESPACE. When an identifier is resolved
/// in a type annotation context, the lookup searches this namespace.
/// 
/// Type declarations include:
///   - Structs (StructDeclAST)
///   - Enums (EnumDeclAST)
///   - Traits (TraitDeclAST)
/// 
/// @note TypeDeclAST nodes are stored in Scope::types map.
struct TypeDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeDecl;
    
    explicit TypeDeclAST(ASTKind k, InternedString n) : DeclAST(k, n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ModuleAST — root node for a single translation unit.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Root node for a source file module.
/// 
/// A ModuleAST represents the entire parsed contents of a single `.luc` file.
/// 
/// ─── Imports Storage ──────────────────────────────────────────────────────
/// 
/// The `imports` field stores the resolved file paths of all imported modules.
/// This is a **derived view** computed during parsing by ModuleResolver.
/// 
/// Why not store ImportDeclAST*?
///   - ImportDeclAST stores the user‑written path (e.g., "io.math")
///   - The CLI and Interpreter need the resolved file path (e.g., "io/math.luc")
///   - The resolved path is the stable key for dependency tracking, JIT module
///     naming, and the file watcher.
/// 
/// The user‑written path (ImportDeclAST::path) is preserved on the AST for:
///   - Error messages (show the user what they wrote)
///   - Alias resolution (ImportDeclAST::alias)
/// 
/// The `imports` field is purely a cache for:
///   - CLI DependencyGraph (build reverse dependencies)
///   - Interpreter ModuleLoader (extract dependencies for hot reload)
///   - LSP (incremental parsing)
/// 
/// @note This field is populated during parsing. It is NOT user input — it is
///       the resolved, canonical path to the imported file.
struct ModuleAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    InternedString filePath;        ///< Resolved file path of this module
    ArenaSpan<DeclAST*> decls;      ///< Top-level declarations
    bool hasErrors = false;

    /// @brief Resolved import paths of this module.
    /// 
    /// These are the resolved file paths (e.g., "io/math.luc", "std/array.luc")
    /// NOT the user‑written import paths (e.g., "io.math", "std.array").
    /// 
    /// Why this is important:
    ///   - The user‑written path is ambiguous (dots vs slashes, no extension)
    ///   - The resolved path is concrete (direct file system mapping)
    ///   - Resolved paths are stable keys across the entire toolchain
    /// 
    /// Populated by: Parser (via ModuleResolver::resolveImportPath())
    /// Used by:      CLI, Interpreter, LSP
    std::vector<InternedString> imports;

    ModuleAST() : BaseAST(ASTKind::Program) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamDeclAST — a generic type parameter declaration.
// ─────────────────────────────────────────────────────────────────────────────

/// 
/// @brief Represents a single generic type parameter declaration.
/// 
/// This node appears in the generic parameter list of functions, structs,
/// and traits. Each parameter has a name and an optional list of trait constraints.
/// 
/// @par Grammar Reference (from LUCID_GRAMMAR.md)
///   generic_param := IDENTIFIER
///                  | IDENTIFIER ':' trait_ref { '+' trait_ref }
/// 
/// @par Examples
///   @code
///   struct Box<T> { ... }                         // unconstrained T
///   const magnitude<T : Vector2> (v T) -> float   // T must implement Vector2
///   struct Pair<A : Named, B : Named> { ... }     // two constrained parameters
///   @endcode
/// 
/// @par Memory Layout (64-bit, typical)
///   - BaseAST overhead    : ~16 bytes (vtable + kind + loc + padding)
///   - `name`              : 4 bytes (InternedString is uint32_t)
///   - `constraints` span  : 16 bytes (ptr + size, each 8 bytes)
///   @n Total: ~36 bytes per generic parameter (excluding constraint nodes)
/// 
/// @par Semantic Resolution
///   During semantic analysis, each constraint type is resolved to a
///   `TraitDeclAST`. The order of constraints does not affect semantics,
///   but is preserved for source fidelity.
/// 
/// @field name        The identifier of the type parameter (e.g., "T", "K", "V").
/// @field constraints Trait types that this parameter must satisfy.
///                    Empty span means the parameter is unconstrained.
///                    Each constraint is a NamedTypeAST node.
/// 
/// @note Multiple constraints are joined with `+` in source (e.g., `T : Vector2 + Named`).
///       The semantic pass verifies that all constraint types resolve to traits
///       and that the traits are compatible.
/// 
struct GenericParamDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParamDecl;

    ArenaSpan<NamedTypeAST*> constraints;   // empty = unconstrained

    explicit GenericParamDeclAST(InternedString n)
        : TypeDeclAST(ASTKind::GenericParamDecl, n) {}
};
using ParamGroup        = std::vector<ParamAST*>;

// ─────────────────────────────────────────────────────────────────────────────
// UnknownAST family — error recovery nodes.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Generic unknown node – fallback when the specific kind is ambiguous.
/// 
/// Used only when the parser cannot determine whether the invalid syntax
/// was a declaration, expression, statement, or type. Prefer the more
/// specific unknown node types when possible.
struct UnknownAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Unknown;
    UnknownAST() : BaseAST(ASTKind::Unknown) {}
};

inline bool isUnknown(BaseAST* node) {
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
    UnknownDeclAST() : DeclAST(ASTKind::UnknownDecl, InternedString()) {}
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