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

#pragma once

#include "debug/DebugMacros.hpp"
#include "../SourceLocation.hpp"
#include "../memory//ASTArena.hpp"
#include "../memory/InternedString.hpp"
#include "../memory//ArenaSpan.hpp"

#include <string>
#include <optional>
#include <memory>
#include <variant>
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
// struct TraitRefAST;

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
    ArrayType,
    NullableType,
    FallibleType,
    CombinedType,      // T?!
    RefType,
    PtrType,
    FuncType,

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
    const T* as() const {
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
    ArenaSpan<AttributeAST*>   attributes;
    InternedString            file;
    bool                      isConst = false;
    InternedString            name;

    explicit DeclAST(ASTKind k) : BaseAST(k) {}
    bool hasDoc() const { return doc.has_value(); }
};

struct TypeAST : BaseAST {
    explicit TypeAST(ASTKind k) : BaseAST(k) {}
};



enum class ValueState {
    None,       // For any call expression that return no value
    Definite,   // Produces a definite value (T)
    Nil,        // Produces nil (T?)
    Err,        // Produces err (T!)
    Unknown,    // Unknown at compile-time (needs runtime evaluation)
};

/// @brief Represents a compile-time constant value.
///
/// This is the result of evaluating a const expression at compile-time.
/// It can represent primitive values, structs, arrays, and function pointers.
///
/// @note All values are immutable after construction.
struct ConstantValue {
    enum class Kind : uint8_t {
        Unknown,    ///< Not yet evaluated
        Error,      ///< Evaluation failed
        Void,       ///< No value (void function)
        Bool,       ///< true/false
        Int,        ///< Integer (any size)
        Float,      ///< Floating point (any precision)
        String,     ///< String literal
        Char,       ///< Character literal
        Enum,       ///< Enum variant
        Struct,     ///< Struct value
        Array,      ///< Array value
        Function,   ///< Const function pointer (for later calls)
        Nil,        ///< nil sentinel
        Err,        ///< err sentinel
    };

    Kind kind = Kind::Unknown;
    TypeAST* type = nullptr;

    // ─── Value Storage ────────────────────────────────────────────────
    // Using variant to store different value types efficiently
    std::variant<
        std::monostate,                                              // Unknown, Error, Void
        bool,                                                        // Bool
        int64_t,                                                     // Int
        double,                                                      // Float
        InternedString,                                              // String, Char, Enum
        std::vector<ConstantValue>,                                  // Array
        std::unordered_map<InternedString, ConstantValue>,           // Struct
        const FuncDeclAST*                                          // Function
    > value;

    // ─── Constructors ──────────────────────────────────────────────────

    ConstantValue() : kind(Kind::Unknown) {}

    explicit ConstantValue(bool v) : kind(Kind::Bool), value(v) {}

    explicit ConstantValue(int64_t v) : kind(Kind::Int), value(v) {}

    explicit ConstantValue(double v) : kind(Kind::Float), value(v) {}

    explicit ConstantValue(InternedString v) : kind(Kind::String), value(v) {}

    explicit ConstantValue(const FuncDeclAST* f) : kind(Kind::Function), value(f) {}

    // ─── Factory Methods ──────────────────────────────────────────────

    static ConstantValue nil() {
        ConstantValue v;
        v.kind = Kind::Nil;
        return v;
    }

    static ConstantValue err() {
        ConstantValue v;
        v.kind = Kind::Err;
        return v;
    }

    static ConstantValue error() {
        ConstantValue v;
        v.kind = Kind::Error;
        return v;
    }

    static ConstantValue voidValue() {
        ConstantValue v;
        v.kind = Kind::Void;
        return v;
    }

    static ConstantValue unknown() {
        return ConstantValue();
    }

    // ─── Predicates ────────────────────────────────────────────────────

    bool isEvaluated() const {
        return kind != Kind::Unknown && kind != Kind::Error;
    }

    bool isError() const {
        return kind == Kind::Error;
    }

    bool isUnknown() const {
        return kind == Kind::Unknown;
    }

    bool isBool() const { return kind == Kind::Bool; }
    bool isInt() const { return kind == Kind::Int; }
    bool isFloat() const { return kind == Kind::Float; }
    bool isString() const { return kind == Kind::String; }
    bool isChar() const { return kind == Kind::Char; }
    bool isVoid() const { return kind == Kind::Void; }
    bool isFunction() const { return kind == Kind::Function; }
    bool isNil() const { return kind == Kind::Nil; }
    bool isErr() const { return kind == Kind::Err; }
    bool isStruct() const { return kind == Kind::Struct; }
    bool isArray() const { return kind == Kind::Array; }
    bool isEnum() const { return kind == Kind::Enum; }

    // ─── Accessors ────────────────────────────────────────────────────

    bool asBool() const {
        return std::get<bool>(value);
    }

    int64_t asInt() const {
        return std::get<int64_t>(value);
    }

    double asFloat() const {
        return std::get<double>(value);
    }

    InternedString asString() const {
        return std::get<InternedString>(value);
    }

    const FuncDeclAST* asFunction() const {
        return std::get<const FuncDeclAST*>(value);
    }

    const std::vector<ConstantValue>& asArray() const {
        return std::get<std::vector<ConstantValue>>(value);
    }

    const std::unordered_map<InternedString, ConstantValue>& asStruct() const {
        return std::get<std::unordered_map<InternedString, ConstantValue>>(value);
    }

    // ─── Mutating Accessors ──────────────────────────────────────────

    std::vector<ConstantValue>& asArrayMut() {
        return std::get<std::vector<ConstantValue>>(value);
    }

    std::unordered_map<InternedString, ConstantValue>& asStructMut() {
        return std::get<std::unordered_map<InternedString, ConstantValue>>(value);
    }

    // ─── Comparison ───────────────────────────────────────────────────

    bool operator==(const ConstantValue& other) const {
        if (kind != other.kind) return false;
        if (type != other.type) return false;
        return value == other.value;
    }

    bool operator!=(const ConstantValue& other) const {
        return !(*this == other);
    }
};

struct ExprAST : BaseAST {
    TypeAST* resolvedType = nullptr; // written as semantic phase
    ValueState valueState = ValueState::Unknown;  // track value state, use to return an `err` value when anything go wrong
    bool isModuleMember   = false;
    bool isConst          = false;
    ConstantValue constValue;  // Evaluated constant value (for const expressions)

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
    
    // Convenience methods
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
    ArenaSpan<LiteralExprAST*> args;  // ← Uses LiteralExprAST directly

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
};
using AttributePtr = AttributeAST*;

// ─────────────────────────────────────────────────────────────────────────────
// ValueDeclAST – base for declarations that produce values
// ─────────────────────────────────────────────────────────────────────────────

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
/// ─── Type Cache ─────────────────────────────────────────────────────────────
/// The `type` field caches the resolved type of this value. For example:
///   - For a variable: its declared type
///   - For a function: its function type (FuncTypeAST)
///   - For a parameter: its parameter type
/// 
/// This eliminates the need for a separate symbol table entry.
/// 
/// @note ValueDeclAST nodes are stored in Scope::values map.
struct ValueDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ValueDecl;

    TypeAST* type = nullptr;
    
    explicit ValueDeclAST(ASTKind k) : DeclAST(k) {}
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
/// ─── Self‑Type Cache ────────────────────────────────────────────────────────
/// The `selfType` field caches a NamedTypeAST that represents this type itself.
/// This is used when a type name appears as a value (e.g., `int("42")` where `int`
/// is used as a conversion function). Without this cache, we would need to
/// create a new NamedTypeAST every time a type name is referenced.
/// 
/// @note TypeDeclAST nodes are stored in Scope::types map.
struct TypeDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeDecl;
    
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

/// @brief Root node for a single translation unit (source file).
/// 
/// This node represents an entire `.luc` file after parsing. It owns all
/// top‑level declarations and provides file‑level context for semantic passes.
/// 
/// @par Memory Layout (64-bit, typical)
///   - BaseAST overhead        : ~20 bytes (vtable + kind + loc + flags + padding)
///   - `packageName`           : 4 bytes (InternedString is a uint32_t)
///   - `filePath`              : 4 bytes (InternedString)
///   - `decls` (ArenaSpan)     : 16 bytes (ptr + size, each 8 bytes)
///   @n Total: ~44 bytes per file (excluding the actual declaration nodes)
/// 
/// @note Why separate `packageName` and `filePath`?
///   - `packageName` is the identifier after `package` (e.g., "math").
///     Used for cross‑file symbol resolution within the same package.
///   - `filePath` is the relative path from the package root (e.g., "math/vec2.luc").
///     Used for error messages, debug info, and module identity.
///   Both are interned to avoid duplicate string storage across the AST.
/// 
/// @par Declaration Ownership
///   The `decls` span holds all top‑level declarations in source order.
///   Each declaration is an ASTPtr<DeclAST> (unique_ptr with no‑op deleter).
///   The underlying memory is arena‑allocated; the unique_ptr is just an
///   ownership wrapper that does not call delete.
/// 
/// @field packageName The package name declared by `package foo` at file start.
/// @field filePath    Relative path from package root (e.g., "math/vec2.luc").
/// @field decls       Top‑level declarations in source order.
/// 
/// @note Diagnostics for this module are NOT stored here. They live in the
///       `diagnostic` namespace's own whole-session list (see
///       Diagnostic.hpp), keyed by `filePath` — get them with
///       `diagnostic::getAllForFile(module->filePath)`. Storing a second
///       copy on the node itself was removed once the diagnostic system
///       started tracking file association on its own (see
///       `diagnostic::pushSource()`/`getAllForFile()`); keeping one here
///       too would just be the same data living in two places again, the
///       exact duplication the diagnostic-system rewrite was meant to
///       eliminate.
/// 
///       `hasErrors` remains as a cheap cached bool — a single flag, set
///       once from `diagnostic::hasErrorsInCurrentSource()` right after
///       this module finishes parsing/analysis, so callers that only need
///       "did this succeed" don't have to make a lookup (or pull in
///       Diagnostic.hpp at all) just to check a yes/no. That's a small
///       derived snapshot, not a duplicate store, which is why it stayed
///       while `errors` didn't.
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
struct GenericParamDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParamDecl;

    InternedString name;
    ArenaSpan<NamedTypeAST*> constraints;   // empty = unconstrained

    explicit GenericParamDeclAST(InternedString n)
        : DeclAST(ASTKind::GenericParamDecl), name(n) {}
};

using ParamPtr          = ParamAST*;
using ParamGroup        = std::vector<ParamPtr>;
using GenericParamDeclPtr   = GenericParamDeclAST*;

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