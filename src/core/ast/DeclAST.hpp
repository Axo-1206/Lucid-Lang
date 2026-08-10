/// @file DeclAST.hpp
/// 
/// @responsibility Defines AST nodes for declarations – entities that introduce
///                 new names into a scope (functions, structs, variables, etc.).
/// 
/// @hierarchy BaseAST → DeclAST → ValueDeclAST/TypeDeclAST → [Concrete Decl Nodes]
/// 
/// @related_files
///   - src/parser/ParserDecl.cpp – primary producer of these nodes
///   - src/semantic/DeclarationCollector.cpp – consumes for scope registration
///   - src/semantic/resolver/TypeResolver.cpp – resolves types and generic parameters
/// 
/// @note Doc comments and attributes are stored in the DeclAST base class,
///       not in every BaseAST node.
/// 
/// ============================================================================
/// NAMESPACE SEPARATION
/// ============================================================================
/// 
/// Declarations are split into two namespaces:
/// 
///   VALUE NAMESPACE (ValueDeclAST):
///     - Variables (VarDeclAST)
///     - Functions (FuncDeclAST)
///     - Parameters (ParamAST)
///     - Fields (FieldDeclAST)
///     - Enum variants (EnumVariantAST)
/// 
///   TYPE NAMESPACE (TypeDeclAST):
///     - Structs (StructDeclAST)
///     - Enums (EnumDeclAST)
///     - Traits (TraitDeclAST)
/// 
/// This separation allows:
///   - `struct Point` and `let Point = 42` to coexist
///   - Faster lookup (search only relevant namespace)
///   - Clearer error messages ("undefined variable" vs "undefined type")
/// 
/// ============================================================================
/// IMMUTABILITY DESIGN
/// ============================================================================
/// 
/// Parser fields are **immutable** after initialization (const in C++).
/// Semantic and CodeGen fields are **mutable** (set during later phases).
/// 
/// This design ensures:
///   1. The parser's output is fixed and cannot be accidentally modified.
///   2. Semantic analysis can augment nodes without corrupting parse results.
///   3. CodeGen can annotate nodes for code generation.
/// 
/// ## Field Categories
/// 
/// | Category        | Mutability          | Set By  | Examples                       |
/// | --------------- | ------------------- | ------- | ------------------------------ |
/// | Parser Fields   | `const` (immutable) | Parser  | `name`, `type`, `init`, `body` |
/// | Semantic Fields | `mutable`           | Sema    | `resolvedType`, `constValue`   |
/// | CodeGen Fields  | `mutable`           | CodeGen | `llvmValue`, `llvmFunction`    |
/// 
/// ## Constructor Pattern
/// 
/// All declaration nodes use constructor initialization for parser fields:
/// 
/// ```cpp
/// struct VarDeclAST : ValueDeclAST {
///     // Parser fields - const (set once in constructor)
///     const DeclKeyword keyword;
///     const TypePtr type;
///     const ExprPtr init;
///     
///     // CodeGen fields - mutable
///     llvm::AllocaInst* llvmAlloca = nullptr;
///     llvm::GlobalVariable* llvmGlobal = nullptr;
///     
///     VarDeclAST(InternedString n, DeclKeyword kw, TypePtr t, ExprPtr i)
///         : ValueDeclAST(ASTKind::VarDecl, n)
///         , keyword(kw)
///         , type(t)
///         , init(i) {}
/// };
/// ```

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <memory>
#include <optional>

// ─── LLVM Headers ──────────────────────────────────────────────────────────
// These are needed for CodeGen annotation fields. Parser and Sema don't use
// these fields, but including the headers is fine because they're already
// included indirectly via ExprAST.hpp -> llvm/IR/Intrinsics.h.
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

// ─── ImportDeclAST ─────────────────────────────────────────────────────────

/// @brief Represents a `import` declaration – imports symbols from another module.
/// 
/// @example
///   import std.io                → path = "std.io",      alias = std::io
///   import std.math as math      → path = "std.math",    alias = "math"
///   import graphics.gl as gl     → path = "graphics.gl", alias = "gl"
/// 
/// Path segments are split on '.'. The semantic pass joins them back when
/// resolving against the package root.
/// 
/// @note NOT a ValueDeclAST or TypeDeclAST – imports are handled by the
///       module loader, not by normal scope lookup.
struct ImportDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImportDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString path;
    const InternedString alias;

    // ─── Constructor ─────────────────────────────────────────────────────
    ImportDeclAST(InternedString p, InternedString a)
        : DeclAST(ASTKind::ImportDecl, a)
        , path(p)
        , alias(a) {}
};
using ImportDeclPtr = ImportDeclAST*;

// ─── VarDeclAST ───────────────────────────────────────────────────────────

/// @brief Represents a variable declaration with an explicit type annotation.
/// 
/// @example
///   let count int     = 0
///   const PI float    = 3.14159
///   let name string?  = nil
/// 
/// Type annotation is always required in Lucid – `type` is never null.
/// `init` is null when no initialiser was written (valid for `let` only;
/// `const` must always have an initialiser – enforced by semantic pass).
/// 
/// @note `@[export]` on a variable makes it read-only from outside the module.
struct VarDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const TypeAST* type;
    ExprAST* init;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::AllocaInst* llvmAlloca = nullptr;      // Local variable
    llvm::GlobalVariable* llvmGlobal = nullptr;  // Module-level variable

    // ─── Constructor ─────────────────────────────────────────────────────
    VarDeclAST(InternedString n, DeclKeyword kw, const TypeAST* t, ExprAST* i)
        : ValueDeclAST(ASTKind::VarDecl, n, kw)
        , type(t)
        , init(i) {}
};
using VarDeclPtr = VarDeclAST*;

// ─── ParamAST ─────────────────────────────────────────────────────────────

/// @brief Represents a function parameter.
/// 
/// @example
///   In `const add (a int)(b int) -> int`, `a` and `b` are ParamAST nodes.
/// 
/// Parameters are passed by value (a copy) by default. A `const` parameter
/// marks a read-only reference – the function sees the caller's original value
/// but cannot modify it.
/// 
/// @field type        The parameter type (never null).
/// @field isVariadic  True if this is a variadic parameter (`...type`).
/// @field isConst     True if this is a read-only reference parameter (`const type`).
/// 
/// @note A variadic parameter must be the last parameter in its own param group.
///       Variadic parameters collect trailing arguments into a `[*]type` array.
struct ParamAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const TypeAST* type;
    const bool isVariadic;
    const bool isConstParam;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::Value* llvmValue = nullptr;           // The LLVM argument value
    llvm::AllocaInst* llvmAlloca = nullptr;     // The alloca for the param
    llvm::Type* llvmType = nullptr;             // LLVM type (may differ from AST type)
    size_t abiRegisterIndex = 0;                // Register index for ABI
    bool isByVal = false;                       // If passed by value (structs)

    // ─── Constructor ─────────────────────────────────────────────────────
    ParamAST(InternedString n, const TypeAST* t, bool variadic = false, bool isConstParam = false)
        : ValueDeclAST(ASTKind::Param, n, DeclKeyword::Let)
        , type(t)
        , isVariadic(variadic)
        , isConstParam(isConstParam) {}
};
using ParamPtr = ParamAST*;
using ParamGroup = std::vector<ParamPtr>;

// ─── FuncDeclAST ──────────────────────────────────────────────────────────

/// @brief Represents a function declaration.
/// 
/// @example
///   const add (a int)(b int) -> int = { return a + b }
///   const makeAdder (base int) -> (int) -> int = { ... }
///   const sum (nums ...int) -> int = { ... }
/// 
/// @field keyword              Let or Const (const functions cannot be reassigned)
/// @field genericParams        Generic type parameters (empty if none)
/// @field funcType             Full function type (includes parameter groups and return types)
/// @field body                 Function body (always BlockStmtAST, expression bodies desugared)
/// 
/// @note Visibility is only meaningful at top‑level; inside blocks, declarations
///       are always private. Attributes (e.g., @[export], @[inline]) are stored
///       in DeclAST::attributes.
struct FuncDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const ArenaSpan<GenericParamDeclPtr> genericParams;
    const FuncTypeAST* funcType;
    const StmtAST* body;
    
    // ─── Semantic Annotations (set by Sema) ────────────────────────────
    bool isForeignFunction = false;      // True if @[foreign] attribute is present
    InternedString mangledName;          // Mangled name for AOT compilation
    size_t closureDepth = 0;             // Depth of nesting for closure naming
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::Function* llvmFunction = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    FuncDeclAST(InternedString n, DeclKeyword kw, 
                ArenaSpan<GenericParamDeclPtr> params,
                const FuncTypeAST* ft, const StmtAST* b)
        : ValueDeclAST(ASTKind::FuncDecl, n, kw)
        , genericParams(params)
        , funcType(ft)
        , body(b) {}
};
using FuncDeclPtr = FuncDeclAST*;

// ─── EnumVariantAST ───────────────────────────────────────────────────────

/// @brief Represents one variant of an enum with an explicit value.
/// 
/// @example
///   North = 0    → explicitValue = 0
///   East  = 1    → explicitValue = 1
///   South = 2    → explicitValue = 2
///   West  = 3    → explicitValue = 3
/// 
/// The semantic pass computes final integer values and verifies no duplicates.
/// Values are required in Lucid – no auto-increment (same no-inference stance
/// as variable declarations).
/// 
/// @note Enum variants are accessed as `Direction.North` in source.
///       They live in the value namespace of the enum's scope.
struct EnumVariantAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumVariant;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const int64_t value;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::ConstantInt* llvmValue = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    EnumVariantAST(InternedString n, int64_t v)
        : ValueDeclAST(ASTKind::EnumVariant, n, DeclKeyword::Const)  // Enum variants are always const
        , value(v) {}
};
using EnumVariantPtr = EnumVariantAST*;

// ─── FieldDeclAST ─────────────────────────────────────────────────────────

/// @brief Represents a struct field, optionally with a default value and const-ness.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// The semantic pass must enforce the following rules:
/// 1. **No Nullable/Fallible**: If `isConst` is true, `type` must not be
///    `NullableTypeAST` or `FallibleTypeAST`. Emit a compile error if it is.
/// 2. **Assignment Rejection**: Any assignment to a `const` field through
///    field access (`struct.field = value`) must be rejected with a compile error.
/// 3. **Deep Immutability**: `const` on struct declaration is not transitive 
///    to inner struct fields.
/// NOTE: the default value rule is the same for both const/let keywords
///       the const keyword enforce immutable after declarartion, default
///       value will override the default value, the declared keyword does not
///       matter here.
struct FieldDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const TypeAST* type;
    const StmtAST* defaultBody;
    const bool isConstField;
    ExprAST* defaultVal;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    size_t fieldIndex = 0;       // Position in struct layout
    llvm::Type* llvmType = nullptr;  // LLVM type of this field
    uint64_t byteOffset = 0;     // Byte offset from struct start

    FieldDeclAST(InternedString n, const TypeAST* t, ExprAST* dv, const StmtAST* db, bool isConstField)
        : ValueDeclAST(ASTKind::FieldDecl, n, DeclKeyword::Let)
        , type(t)
        , defaultVal(dv)
        , defaultBody(db)
        , isConstField(isConstField) {}
    
    bool isConst() const { return isConstField; }
};
using FieldDeclPtr = FieldDeclAST*;

// ─── StructDeclAST ────────────────────────────────────────────────────────

/// @brief Represents a struct definition with fields and optional generic parameters.
/// 
/// @example
///   struct Point { x float = 0.0, y float = 0.0 }
///   struct Node<T> { value T, next ptr<Node<T>>? }
///   struct Entity : Vector2, Named { name string, x float, y float, health int }
/// 
/// A struct may implement one or more traits by listing them after `:`.
/// The traits are stored in `traitRefs` and resolved during semantic analysis.
/// 
/// @field genericParams  Generic type parameters (empty if none)
/// @field fields         Struct fields (may include const fields)
/// @field traitRefs      Traits this struct implements (empty if none)
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// The semantic pass must enforce the following rules for struct declarations:
/// 1. **Trait Implementation**: For each trait in `traitRefs`, verify that the
///    struct declares all fields from the trait with matching names and types.
/// 2. **Const Matching**: If a trait field is marked `const`, the struct's
///    corresponding field must also be marked `const`. If the struct declares
///    it as mutable, emit a compile error.
/// 3. **Type Matching**: All trait fields must have matching types in the
///    implementing struct. Type mismatch is a compile error.
/// 4. **Const Conflict Resolution**: If a struct implements multiple traits,
///    if there are fields that have the same name then it is an compile error
/// 5. **Generic Parameters**: All generic parameters must be used in at least
///    one field type. Unused parameters are a compile error.
/// 6. **No Reference Fields**: Fields cannot have reference type (`&T`).
///    This is enforced by the type system.
struct StructDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const ArenaSpan<GenericParamDeclPtr> genericParams;
    const ArenaSpan<FieldDeclPtr> fields;
    const ArenaSpan<NamedTypeAST*> traitRefs;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::StructType* llvmType = nullptr; // The generated LLVM struct type
    uint64_t totalSize = 0;               // Total size in bytes
    uint64_t alignment = 0;               // Required alignment
    bool isPacked = false;                // If @[packed] attribute is present

    // ─── Constructor ─────────────────────────────────────────────────────
    StructDeclAST(InternedString n,
                  ArenaSpan<GenericParamDeclPtr> params,
                  ArenaSpan<FieldDeclPtr> flds,
                  ArenaSpan<NamedTypeAST*> traits)
        : TypeDeclAST(ASTKind::StructDecl, n)
        , genericParams(params)
        , fields(flds)
        , traitRefs(traits) {}
    
    size_t indexOfField(InternedString name) const {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i]->name == name) return i;
        }
        return SIZE_MAX;
    }
};
using StructDeclPtr = StructDeclAST*;
using StructDeclPtr = StructDeclAST*;

// ─── EnumDeclAST ──────────────────────────────────────────────────────────

/// @brief Represents an enum definition.
/// 
/// @example
///   enum Direction { North = 0, East = 1, South = 2, West = 3 }
///   enum Status : int32 { Ok = 200, NotFound = 404, Error = 500 }
/// 
/// Each variant must have an explicit integer value. Values are required
/// (no auto-increment) – this matches the no-inference stance applied
/// everywhere else in the grammar.
/// 
/// @field variants      Enum variants with their explicit values
/// @field backingType   Optional backing integer type (defaults to int32)
struct EnumDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const ArenaSpan<EnumVariantPtr> variants;
    const PrimitiveTypeAST* backingType;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    ArenaSpan<llvm::ConstantInt*> variantConstants;  // Parallel to variants span
    llvm::IntegerType* backingLLVMType = nullptr;    // LLVM type of backing int
    uint64_t byteSize = 0;                           // Size of enum in bytes

    llvm::ConstantInt* constantForVariant(InternedString name) const {
        for (size_t i = 0; i < variants.size(); ++i) {
            if (variants[i]->name == name) return variantConstants[i];
        }
        return nullptr;
    }

    // ─── Constructor ─────────────────────────────────────────────────────
    EnumDeclAST(InternedString n,
                ArenaSpan<EnumVariantPtr> vars,
                const PrimitiveTypeAST* backing = nullptr)
        : TypeDeclAST(ASTKind::EnumDecl, n)
        , variants(vars)
        , backingType(backing) {}
};
using EnumDeclPtr = EnumDeclAST*;

// ─── TraitFieldDeclAST ────────────────────────────────────────────────────

/// @brief Represents a single field requirement in a trait declaration.
/// 
/// A trait is a pure **field contract** – a named set of fields (name, type,
/// and optional const-ness) that a struct promises to contain. Traits have no
/// methods, no behavior, no qualifiers, and no default values.
/// 
/// @example
///   trait Vector2 { x float, y float }
///   trait Named { name string }
///   trait Container<T> { value T, count int }
///   trait ImmutableConfig { const maxRetries int, const timeout float }
///   trait NullableContainer { value int?, fallback int? }  // nullable allowed
///   trait ErrorHandler { result string!, fallback string! } // fallible allowed
/// 
/// @field name      The required field name.
/// @field type      The required field type (may be nullable or fallible unless const).
/// @field isConst   True if the implementing struct must declare this field as `const`.
/// 
/// ─── Trait Field Rules ──────────────────────────────────────────────────────
/// 1. **Name and Type Only**: Trait fields declare name, type, and optional
///    const-ness – no default values. Qualifiers and defaults belong to the
///    implementing struct.
/// 
/// 2. **Const Requirement**: If `isConst` is true, the implementing struct
///    MUST declare this field as `const`.
/// 
/// 3. **Type Restrictions**: 
///    - If `isConst` is true, the field type MUST be definite (not nullable or fallible).
///    - If `isConst` is false, the field type MAY be nullable (`T?`), fallible (`T!`), 
///      or combined (`T?!`). This allows traits to require optional or error-prone fields.
/// 
/// 4. **Self-Reference**: Trait fields can reference the trait itself via its name.
///    This enables recursive trait definitions.
/// 
/// @note Not a ValueDeclAST because trait fields are requirements, not
///       actual values. The semantic pass uses them to verify that implementing
///       structs declare all required fields with matching names, types, and const-ness.
struct TraitFieldDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitFieldDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const InternedString name;
    const TypeAST* type; // required field type (nullable/fallible allowed unless const)
    const bool isConstField;

    // ─── Constructor ─────────────────────────────────────────────────────
    TraitFieldDeclAST(InternedString n, const TypeAST* t, bool isConstField)
        : DeclAST(ASTKind::TraitFieldDecl, n)
        , name(n)
        , type(t) 
        , isConstField(isConstField) {}

    /// @brief Check if this field is const (immutable after construction).
    bool isConst() const { return isConstField; }
};
using TraitFieldPtr = TraitFieldDeclAST*;

// ─── TraitDeclAST ─────────────────────────────────────────────────────────

/// @brief Represents a trait – a named set of fields that a struct promises to contain.
/// 
/// @example
///   trait Vector2 { x float, y float }
///   trait Named { name string }
///   trait Container<T> { value T, count int }
///   trait ImmutableConfig { const maxRetries int, const timeout float }
/// 
/// Used by the semantic pass to:
///   - Verify that a struct implementing a trait declares all required fields
///   - Serve as constraints in generic parameter declarations (`<T : Trait>`)
///   - Check field type and const-ness compatibility (mismatch is a compile error)
/// 
/// ## Generic Traits
/// 
/// Traits can be generic. Generic arguments are resolved at the constraint site:
///   `<T : Container<int>>` means T must implement Container with int.
/// 
/// @field genericParams  Generic type parameters (empty if none)
/// @field fields         Required field declarations (name + type + optional const)
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// The semantic pass must enforce the following rules for trait declarations:
/// 1. **Trait Field Name Uniqueness**: All field names within a trait must be
///    unique. Duplicate names with different types are a compile error.
/// 2. **Generic Parameters**: All generic parameters must be used in at least
///    one field type. Unused parameters are a compile error.
/// 3. **No Trait Inheritance**: Traits do not inherit from other traits.
/// 4. **No default value**
struct TraitDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const ArenaSpan<GenericParamDeclPtr> genericParams;
    const ArenaSpan<TraitFieldPtr> fields;

    // ─── Constructor ─────────────────────────────────────────────────────
    TraitDeclAST(InternedString n,
                 ArenaSpan<GenericParamDeclPtr> params,
                 ArenaSpan<TraitFieldPtr> flds)
        : TypeDeclAST(ASTKind::TraitDecl, n)
        , genericParams(params)
        , fields(flds) {}
};
using TraitDeclPtr = TraitDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// Aliases for common pointer types.
// ─────────────────────────────────────────────────────────────────────────────

using ImportDeclPtr = ImportDeclAST*;
using VarDeclPtr = VarDeclAST*;
using ParamPtr = ParamAST*;
using FuncDeclPtr = FuncDeclAST*;
using FieldDeclPtr = FieldDeclAST*;
using StructDeclPtr = StructDeclAST*;
using EnumVariantPtr = EnumVariantAST*;
using EnumDeclPtr = EnumDeclAST*;
using TraitFieldPtr = TraitFieldDeclAST*;
using TraitDeclPtr = TraitDeclAST*;