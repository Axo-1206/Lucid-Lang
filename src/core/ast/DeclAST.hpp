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
/// FIELD CATEGORIES
/// ============================================================================
/// 
/// | Category        | Mutability          | Set By  | Examples                                    |
/// | --------------- | ------------------- | ------- | ------------------------------------------- |
/// | Parser Fields   | `const` (immutable) | Parser  | `name`, `type`, `init`, `body`              |
/// | Semantic Fields | `mutable`           | Sema    | `resolvedType`, `mangledName`, `closureDepth` |
/// | Layout Fields   | `mutable`           | Sema    | `fieldIndex`, `byteOffset`, `totalSize`     |
/// | CodeGen Fields  | `mutable`           | CodeGen | `llvmFunction`, `llvmType`, `llvmAlloca`    |
/// 
/// ## Constructor Pattern
/// 
/// All declaration nodes use constructor initialization for parser fields:
/// 
/// ```cpp
/// struct VarDeclAST : ValueDeclAST {
///     // Parser fields - const (set once in constructor)
///     TypeAST* type;
///     ExprAST* init;
///     
///     // CodeGen fields - mutable
///     llvm::AllocaInst* llvmAlloca = nullptr;
///     llvm::GlobalVariable* llvmGlobal = nullptr;
///     
///     VarDeclAST(InternedString n, DeclKeyword kw, TypeAST* t, ExprAST* i)
///         : ValueDeclAST(ASTKind::VarDecl, n, kw)
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

    ExprAST* init;

    // ─── Semantic Fields (set by Sema) ────────────────────────────────
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    InternedString mangledName;        // Mangled name for AOT compilation
    llvm::AllocaInst* llvmAlloca = nullptr;      // Local variable alloca
    llvm::GlobalVariable* llvmGlobal = nullptr;  // Module-level global

    // ─── Constructor ─────────────────────────────────────────────────────
    VarDeclAST(InternedString n, DeclKeyword kw, TypeAST* t, ExprAST* i)
        : ValueDeclAST(ASTKind::VarDecl, n, kw, t)
        , init(i) {}
};

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
/// @field isConstParam True if this is a read-only reference parameter (`const type`).
/// 
/// @note A variadic parameter must be the last parameter in its own param group.
///       Variadic parameters collect trailing arguments into a `[*]type` array.
struct ParamAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const bool isVariadic;        // True if variadic (`...type`)
    const bool isConstParam;      // True if read-only reference (`const type`)
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::Value* llvmValue = nullptr;          // The LLVM argument value
    llvm::AllocaInst* llvmAlloca = nullptr;    // The alloca for the param
    llvm::Type* llvmType = nullptr;            // LLVM type (may differ from AST type)
    size_t abiRegisterIndex = 0;               // Register index for ABI
    bool isByVal = false;                      // If passed by value (structs)

    // ─── Constructor ─────────────────────────────────────────────────────
    ParamAST(InternedString n, TypeAST* t, bool variadic = false, bool isConstParam = false)
        : ValueDeclAST(ASTKind::Param, n, DeclKeyword::Let, t)  // Parameters are always Let by default
        , isVariadic(variadic)
        , isConstParam(isConstParam) {}
};
using ParamGroup = std::vector<ParamAST*>;

// ─── FuncDeclAST ──────────────────────────────────────────────────────────

/// @brief Represents a function declaration.
/// 
/// @example
///   const add (a int)(b int) -> int = { return a + b }
///   const makeAdder (base int) -> (int) -> int = { ... }
///   const sum (nums ...int) -> int = { ... }
/// 
/// ─── Closures ──────────────────────────────────────────────────────────────
/// A FuncDeclAST can also be a closure if it captures variables from its
/// enclosing scope. When this happens, the function behaves like an
/// anonymous function with a name.
/// 
/// Example of a nested function that forms a closure:
/// ```lucid
/// const makeCounter () -> () -> int = {
///     let count int = 0;
///     const counter () -> int = {   ← This is a FuncDeclAST that captures 'count'
///         count = count + 1;
///         return count;
///     };
///     return counter;
/// }
/// ```
struct FuncDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    FuncTypeAST* funcType = nullptr;
    StmtAST* body;
    
    // ─── Semantic Fields (set by Sema) ────────────────────────────────
    bool isForeignFunction = false;    // True if @[foreign] attribute is present
    bool shouldSpecialize = false;     // from @[specialize]
    bool isInline = false;             // from @[inline]
    bool isNoInline = false;           // from @[noinline]
    
    /// Variables captured by this function (if it's a closure).
    /// Populated by capture analysis during semantic analysis.
    ArenaSpan<CapturedVariable> captures;
    bool hasClosure = false;    /// True if this function captures any variables from outer scopes.
    bool isReturned = false;    /// True if this function is returned from its parent
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    InternedString mangledName;        // Mangled name for AOT compilation
    llvm::Function* llvmFunction = nullptr;
    
    /// The LLVM struct type for the closure environment (if this is a closure).
    llvm::StructType* environmentType = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    FuncDeclAST(InternedString n, DeclKeyword kw, 
                ArenaSpan<GenericParamDeclAST*> params,
                FuncTypeAST* ft, StmtAST* b)
        : ValueDeclAST(ASTKind::FuncDecl, n, kw, ft)
        , funcType(ft)
        , genericParams(params)
        , body(b) {}
};

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
    const int64_t value;              // Explicit integer value
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::ConstantInt* llvmValue = nullptr;

    // ─── Constructor ─────────────────────────────────────────────────────
    EnumVariantAST(InternedString n, int64_t v)
        : ValueDeclAST(ASTKind::EnumVariant, n, DeclKeyword::Const, nullptr)  // Enum variants are always const
        , value(v) {}
};

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
/// 
/// NOTE: the default value rule is the same for both const/let keywords.
///       The const keyword enforces immutability after declaration, but the
///       default value will override the default value. The declared keyword
///       does not matter for default values.
struct FieldDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    StmtAST* defaultBody;   // Block body default (for function fields)
    const bool isConstField;      // True if field is marked `const` in struct

    ExprAST* defaultVal = nullptr;

    // ─── Semantic Fields (set by Sema) ────────────────────────────────
    size_t fieldIndex = 0;        // Position in struct layout
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::Type* llvmType = nullptr;   // LLVM type of this field
    uint64_t byteOffset = 0;          // Byte offset (from LLVM DataLayout)

    // ─── Constructor ─────────────────────────────────────────────────────
    FieldDeclAST(InternedString n, TypeAST* t, ExprAST* dv, 
                 StmtAST* db, bool isConstField)
        : ValueDeclAST(ASTKind::FieldDecl, n, DeclKeyword::Let, t)
        , defaultBody(db)
        , isConstField(isConstField)
        , defaultVal(dv) {}
    
    bool isConst() const { return isConstField; }
};

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
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// The semantic pass must enforce the following rules for struct declarations:
/// 1. **Trait Implementation**: For each trait in `traitRefs`, verify that the
///    struct declares all fields from the trait with matching names and types.
/// 2. **Const Matching**: If a trait field is marked `const`, the struct's
///    corresponding field must also be marked `const`.
/// 3. **Type Matching**: All trait fields must have matching types.
/// 4. **Const Conflict Resolution**: If multiple traits require the same field
///    name with different const-ness, it's a compile error.
/// 5. **Generic Parameters**: All generic parameters must be used in at least
///    one field type. Unused parameters are a compile error.
/// 6. **No Reference Fields**: Fields cannot have reference type (`&T`).
struct StructDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    ArenaSpan<FieldDeclAST*> fields;
    ArenaSpan<NamedTypeAST*> traitRefs;
    const bool isPacked = false;  // From @[packed] attribute
    
    // ─── Semantic Fields (set by Sema) ────────────────────────────────
    bool shouldSpecialize = false;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    llvm::StructType* llvmType = nullptr;
    InternedString mangledName;        // Mangled name for AOT compilation
    
    // Physical layout - computed by CodeGen using LLVM DataLayout
    uint64_t totalSize = 0;
    uint64_t alignment = 0;

    // ─── Constructor ─────────────────────────────────────────────────────
    StructDeclAST(InternedString n,
                  ArenaSpan<GenericParamDeclAST*> params,
                  ArenaSpan<FieldDeclAST*> flds,
                  ArenaSpan<NamedTypeAST*> traits,
                  bool packed = false)
        : TypeDeclAST(ASTKind::StructDecl, n)
        , genericParams(params)
        , fields(flds)
        , traitRefs(traits)
        , isPacked(packed) {}
    
    size_t indexOfField(InternedString name) const {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i]->name == name) return i;
        }
        return SIZE_MAX;
    }
};

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
// In DeclAST.hpp
struct EnumDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<EnumVariantAST*> variants;
    PrimitiveTypeAST* backingType;
    
    // ─── CodeGen Fields (mutable) ──────────────────────────────────────
    // Changed from ArenaSpan to std::vector because LLVM objects
    // are not allocated in the AST arena.
    std::vector<llvm::ConstantInt*> variantConstants;
    llvm::IntegerType* backingLLVMType = nullptr;
    uint64_t byteSize = 0;
    InternedString mangledName;

    // ─── Constructor ─────────────────────────────────────────────────────
    EnumDeclAST(InternedString n,
                ArenaSpan<EnumVariantAST*> vars,
                PrimitiveTypeAST* backing = nullptr)
        : TypeDeclAST(ASTKind::EnumDecl, n)
        , variants(vars)
        , backingType(backing) {}
    
    // ─── Helper to find variant constant by name ──────────────────────
    llvm::ConstantInt* constantForVariant(InternedString name) const {
        for (size_t i = 0; i < variants.size(); ++i) {
            if (variants[i]->name == name) {
                return i < variantConstants.size() ? variantConstants[i] : nullptr;
            }
        }
        return nullptr;
    }
};

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
/// ─── Trait Field Rules ──────────────────────────────────────────────────────
/// 1. **Name and Type Only**: Trait fields declare name, type, and optional
///    const-ness – no default values.
/// 2. **Const Requirement**: If `isConst` is true, the implementing struct
///    MUST declare this field as `const`.
/// 3. **Type Restrictions**: 
///    - If `isConst` is true, the field type MUST be definite (not nullable or fallible).
///    - If `isConst` is false, the field type MAY be nullable (`T?`), fallible (`T!`), 
///      or combined (`T?!`).
/// 4. **Self-Reference**: Trait fields can reference the trait itself via its name.
/// 
/// @note Not a ValueDeclAST because trait fields are requirements, not
///       actual values. The semantic pass uses them to verify that implementing
///       structs declare all required fields.
struct TraitFieldDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitFieldDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    TypeAST* type;          // Required field type
    const bool isConstField;      // True if implementing struct must declare as const

    // ─── Constructor ─────────────────────────────────────────────────────
    TraitFieldDeclAST(InternedString n, TypeAST* t, bool isConstField)
        : DeclAST(ASTKind::TraitFieldDecl, n)
        , type(t) 
        , isConstField(isConstField) {}

    bool isConst() const { return isConstField; }
};

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
///   - Check field type and const-ness compatibility
/// 
/// ## Generic Traits
/// 
/// Traits can be generic. Generic arguments are resolved at the constraint site:
///   `<T : Container<int>>` means T must implement Container with int.
/// 
/// ─── Semantic Analysis Notes ──────────────────────────────────────────────
/// The semantic pass must enforce the following rules for trait declarations:
/// 1. **Trait Field Name Uniqueness**: All field names within a trait must be
///    unique. Duplicate names with different types are a compile error.
/// 2. **Generic Parameters**: All generic parameters must be used in at least
///    one field type. Unused parameters are a compile error.
/// 3. **No Trait Inheritance**: Traits do not inherit from other traits.
/// 4. **No Default Values**: Traits define field requirements only.
struct TraitDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    ArenaSpan<TraitFieldDeclAST*> fields;

    // ─── Constructor ─────────────────────────────────────────────────────
    TraitDeclAST(InternedString n,
                 ArenaSpan<GenericParamDeclAST*> params,
                 ArenaSpan<TraitFieldDeclAST*> flds)
        : TypeDeclAST(ASTKind::TraitDecl, n)
        , genericParams(params)
        , fields(flds) {}
};