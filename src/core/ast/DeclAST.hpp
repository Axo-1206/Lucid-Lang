/**
 * @file DeclAST.hpp
 *
 * @responsibility Defines AST nodes for declarations – entities that introduce
 *                 new names into a scope (functions, structs, variables, etc.).
 *
 * @hierarchy BaseAST → DeclAST → ValueDeclAST/TypeDeclAST → [Concrete Decl Nodes]
 *
 * @related_files
 *   - src/parser/ParserDecl.cpp – primary producer of these nodes
 *   - src/semantic/DeclarationCollector.cpp – consumes for scope registration
 *   - src/semantic/resolver/TypeResolver.cpp – resolves types and generic parameters
 *
 * @note Doc comments and attributes are stored in the DeclAST base class,
 *       not in every BaseAST node.
 *
 * ============================================================================
 * NAMESPACE SEPARATION
 * ============================================================================
 *
 * Declarations are split into two namespaces:
 *
 *   VALUE NAMESPACE (ValueDeclAST):
 *     - Variables (VarDeclAST)
 *     - Functions (FuncDeclAST)
 *     - Parameters (ParamAST)
 *     - Fields (FieldDeclAST)
 *     - Enum variants (EnumVariantAST)
 *
 *   TYPE NAMESPACE (TypeDeclAST):
 *     - Structs (StructDeclAST)
 *     - Enums (EnumDeclAST)
 *     - Traits (TraitDeclAST)
 *
 * This separation allows:
 *   - `struct Point` and `let Point = 42` to coexist
 *   - Faster lookup (search only relevant namespace)
 *   - Clearer error messages ("undefined variable" vs "undefined type")
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <memory>
#include <optional>
#include <unordered_map>

/**
 * @brief Distinguishes between mutable and immutable declarations.
 *
 * - Let:   mutable binding (can be reassigned)
 * - Const: immutable binding (cannot be reassigned)
 *
 * @note For struct fields, `Const` means the field cannot be reassigned
 *       after construction, even if the containing variable is `let`.
 */
enum class DeclKeyword {
    Let,    // mutable
    Const   // immutable
};

/**
 * @brief Represents a `use` declaration – imports symbols from another module.
 *
 * @example
 *   use std.io                → path = "std.io",      alias = std::io
 *   use std.math as math      → path = "std.math",    alias = "math"
 *   use graphics.gl as gl     → path = "graphics.gl", alias = "gl"
 *
 * Path segments are split on '.'. The semantic pass joins them back when
 * resolving against the package root.
 *
 * @note NOT a ValueDeclAST or TypeDeclAST – imports are handled by the
 *       module loader, not by normal scope lookup.
 */
struct UseDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UseDecl;

    InternedString path;
    InternedString alias;

    UseDeclAST() : DeclAST(ASTKind::UseDecl) {}
};
using UseDeclPtr = UseDeclAST*;

/**
 * @brief Represents a variable declaration with an explicit type annotation.
 *
 * @example
 *   let count int     = 0
 *   const PI float    = 3.14159
 *   let name string?  = nil
 *
 * Type annotation is always required in Lucid – `type` is never null.
 * `init` is null when no initialiser was written (valid for `let` only;
 * `const` must always have an initialiser – enforced by semantic pass).
 *
 * @note `@[export]` on a variable makes it read-only from outside the module.
 */
struct VarDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    DeclKeyword keyword;
    TypePtr type;
    ExprPtr init;

    VarDeclAST() : ValueDeclAST(ASTKind::VarDecl) {}
};
using VarDeclPtr = VarDeclAST*;

/**
 * @brief Represents a function parameter.
 *
 * @example
 *   In `const add (a int)(b int) -> int`, `a` and `b` are ParamAST nodes.
 *
 * Parameters are passed by value (a copy) by default. A `const` parameter
 * marks a read-only reference – the function sees the caller's original value
 * but cannot modify it.
 *
 * @field type        The parameter type (never null).
 * @field isVariadic  True if this is a variadic parameter (`...type`).
 * @field isConst     True if this is a read-only reference parameter (`const type`).
 *
 * @note A variadic parameter must be the last parameter in its own param group.
 *       Variadic parameters collect trailing arguments into a `[*]type` array.
 */
struct ParamAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    TypePtr type;
    bool isVariadic = false;
    bool isConst = false;    // read-only reference parameter

    ParamAST() : ValueDeclAST(ASTKind::Param) {}
};
using ParamPtr = ParamAST*;
using ParamGroup = std::vector<ParamPtr>;

/**
 * @brief Represents a function declaration.
 *
 * @example
 *   const add (a int)(b int) -> int = { return a + b }
 *   const makeAdder (base int) -> (n int) -> int = { ... }
 *   const sum (nums ...int) -> int = { ... }
 *
 * @field keyword              Let or Const (const functions cannot be reassigned)
 * @field genericParams        Generic type parameters (empty if none)
 * @field funcType             Full function type (includes parameter groups and return types)
 * @field body                 Function body (always BlockStmtAST, expression bodies desugared)
 * @field resolvedReturnType   Cached first return type (set during type resolution)
 *
 * ## Currying and Form 2 `()()` Shorthand
 *
 * The parser desugars Form 2 `()()` shorthand into nested Form 1 functions
 * before building the AST. The `funcType` captures the full curried structure:
 *
 *   const clamp (lo int)(hi int)(v int) -> int
 *   → FuncTypeAST: (lo int) -> (hi int) -> (v int) -> int
 *
 * @note Visibility is only meaningful at top‑level; inside blocks, declarations
 *       are always private. Attributes (e.g., @[export], @[inline]) are stored
 *       in DeclAST::attributes.
 */
struct FuncDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    DeclKeyword keyword;
    ArenaSpan<GenericParamDeclPtr> genericParams;
    FuncTypeAST* funcType = nullptr;   // full function type
    StmtPtr body = nullptr;            // always BlockStmtAST
    
    // Cache for the first return type (for quick access during checking)
    // Set during Phase 2 type resolution
    TypePtr resolvedReturnType = nullptr;

    FuncDeclAST() : ValueDeclAST(ASTKind::FuncDecl) {}
};
using FuncDeclPtr = FuncDeclAST*;

/**
 * @brief Represents one variant of an enum with an explicit value.
 *
 * @example
 *   North = 0    → explicitValue = 0
 *   East  = 1    → explicitValue = 1
 *   South = 2    → explicitValue = 2
 *   West  = 3    → explicitValue = 3
 *
 * The semantic pass computes final integer values and verifies no duplicates.
 * Values are required in Lucid – no auto-increment (same no-inference stance
 * as variable declarations).
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the enum type (the variant's type
 * is the enum itself, not the underlying integer).
 *
 * @note Enum variants are accessed as `Direction.North` in source.
 *       They live in the value namespace of the enum's scope.
 */
struct EnumVariantAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumVariant;

    int64_t value;    // explicit value (required by grammar)

    explicit EnumVariantAST(InternedString n, int64_t v)
        : ValueDeclAST(ASTKind::EnumVariant), value(v) {
        name = n;
    }
};
using EnumVariantPtr = EnumVariantAST*;

/**
 * @brief Represents a struct field, optionally with a default value and const-ness.
 *
 * @example
 *   x     float           → defaultVal = nullptr, isConst = false
 *   r     float = 1.0     → defaultVal = literal 1.0, isConst = false
 *   const step  int       → defaultVal = nullptr, isConst = true
 *   const max   int = 100 → defaultVal = literal 100, isConst = true
 *   items [*]string       → type = DynamicArray(String)
 *
 * ─── Const Fields ───────────────────────────────────────────────────────────
 * A field qualified `const` cannot be reassigned through `field_expr`, even
 * when the containing variable is itself `let`. This is useful for:
 *   - Fixed behavior callbacks
 *   - Immutable configuration values
 *   - Function-typed fields that should not be swapped
 *
 * ─── Const Field Rules ──────────────────────────────────────────────────────
 * 1. A `const` field may have a default value (`= expr`). If it has a default,
 *    it can be omitted during initialization.
 * 2. If a `const` field does NOT have a default value, it must be provided
 *    a value during struct initialization.
 * 3. A `const` field may NOT be nullable (`T?`) or fallible (`T!`).
 * 4. `const` fields cannot be reassigned after construction.
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the field's resolved type.
 *
 *@note
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * The semantic pass must enforce the following rules:
 * 1. **No Nullable/Fallible**: If `isConst` is true, `type` must not be
 *    `NullableTypeAST` or `FallibleTypeAST`. Emit a compile error if it is.
 * 2. **Default Value Optional**: A `const` field may have a default value.
 *    If it has a default, it must be omitted during initialization.
 * 3. **Initialization Required**: If `isConst` is true and `defaultVal` is
 *    nullptr, the field must be provided a value during struct initialization.
 *    If missing, emit a compile error.
 * 4. **Assignment Rejection**: Any assignment to a `const` field through
 *    field access (`struct.field = value`) must be rejected with a compile error.
 * 5. **Override not allow**: A `const` field with a default value must not be
 *    overridden during initialization.
 * 6. **Deep Immutability**: `const` on struct declaration is not transitive 
 *    to inner struct fields.
 */
struct FieldDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    TypePtr type;          // Original type annotation (may contain aliases)
    ExprPtr defaultVal;    // nullptr if no default
    bool isConst = false;  // true if field is marked `const`

    FieldDeclAST() : ValueDeclAST(ASTKind::FieldDecl) {}
};
using FieldDeclPtr = FieldDeclAST*;

/**
 * @brief Represents a struct definition with fields and optional generic parameters.
 *
 * @example
 *   struct Point { x float = 0.0, y float = 0.0 }
 *   struct Node<T> { value T, next ptr<Node<T>>? }
 *   struct Entity : Vector2, Named { name string, x float, y float, health int }
 *
 * A struct may implement one or more traits by listing them after `:`.
 * The traits are stored in `traitRefs` and resolved during semantic analysis.
 *
 * @field genericParams  Generic type parameters (empty if none)
 * @field fields         Struct fields (may include const fields)
 * @field traitRefs      Traits this struct implements (empty if none)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * The semantic pass must enforce the following rules for struct declarations:
 * 1. **Trait Implementation**: For each trait in `traitRefs`, verify that the
 *    struct declares all fields from the trait with matching names and types.
 * 2. **Const Matching**: If a trait field is marked `const`, the struct's
 *    corresponding field must also be marked `const`. If the struct declares
 *    it as mutable, emit a compile error.
 * 3. **Type Matching**: All trait fields must have matching types in the
 *    implementing struct. Type mismatch is a compile error.
 * 4. **Const Conflict Resolution**: If a struct implements multiple traits,
 *    if there are fields that have the same name then it is an compile error
 * 5 **Generic Parameters**: All generic parameters must be used in at least
 *    one field type. Unused parameters are a compile error.
 * 6. **No Reference Fields**: Fields cannot have reference type (`&T`).
 *    This is enforced by the type system.
 */
struct StructDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    ArenaSpan<GenericParamDeclPtr> genericParams;
    ArenaSpan<FieldDeclPtr> fields;
    ArenaSpan<TraitRefAST*> traitRefs;   // traits this struct implements

    StructDeclAST() : TypeDeclAST(ASTKind::StructDecl) {}
};
using StructDeclPtr = StructDeclAST*;

/**
 * @brief Represents an enum definition.
 *
 * @example
 *   enum Direction { North = 0, East = 1, South = 2, West = 3 }
 *   enum Status : int32 { Ok = 200, NotFound = 404, Error = 500 }
 *
 * Each variant must have an explicit integer value. Values are required
 * (no auto-increment) – this matches the no-inference stance applied
 * everywhere else in the grammar.
 *
 * @field variants      Enum variants with their explicit values
 * @field backingType   Optional backing integer type (defaults to int32)
 */
struct EnumDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    ArenaSpan<EnumVariantPtr> variants;
    PrimitiveTypeAST* backingType = nullptr;   // optional backing type (defaults to int32)

    EnumDeclAST() : TypeDeclAST(ASTKind::EnumDecl) {}
};
using EnumDeclPtr = EnumDeclAST*;

/**
 * @brief Represents a single field requirement in a trait declaration.
 *
 * A trait is a pure **field contract** – a named set of fields (name, type,
 * and optional const-ness) that a struct promises to contain. Traits have no
 * methods, no behavior, no qualifiers, and no default values.
 *
 * @example
 *   trait Vector2 { x float, y float }
 *   trait Named { name string }
 *   trait Container<T> { value T, count int }
 *   trait ImmutableConfig { const maxRetries int, const timeout float }
 *
 * @field name      The required field name.
 * @field type      The required field type (must not be nullable or fallible).
 * @field isConst   True if the implementing struct must declare this field as `const`.
 *
 * ─── Trait Field Rules ──────────────────────────────────────────────────────
 * 1. **Name and Type Only**: Trait fields declare name, type, and optional
 *    const-ness – no default values. Qualifiers and defaults belong to the
 *    implementing struct.
 * 2. **Const Requirement**: If `isConst` is true, the implementing struct
 *    MUST declare this field as `const`.
 *
 * @note Not a ValueDeclAST because trait fields are requirements, not
 *       actual values. The semantic pass uses them to verify that implementing
 *       structs declare all required fields with matching names, types, and const-ness.
 */
struct TraitFieldDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitFieldDecl;

    InternedString name;
    TypePtr type;          // required field type (must not be nullable/fallible)
    bool isConst = false;  // true if implementing struct must declare as const

    TraitFieldDeclAST() : DeclAST(ASTKind::TraitFieldDecl) {}
};
using TraitFieldPtr = TraitFieldDeclAST*;

/**
 * @brief Represents a trait – a named set of fields that a struct promises to contain.
 *
 * @example
 *   trait Vector2 { x float, y float }
 *   trait Named { name string }
 *   trait Container<T> { value T, count int }
 *   trait ImmutableConfig { const maxRetries int, const timeout float }
 *
 * Used by the semantic pass to:
 *   - Verify that a struct implementing a trait declares all required fields
 *   - Serve as constraints in generic parameter declarations (`<T : Trait>`)
 *   - Check field type and const-ness compatibility (mismatch is a compile error)
 *
 * ## Generic Traits
 *
 * Traits can be generic. Generic arguments are resolved at the constraint site:
 *   `<T : Container<int>>` means T must implement Container with int.
 *
 * @field genericParams  Generic type parameters (empty if none)
 * @field fields         Required field declarations (name + type + optional const)
 *
 * ─── Semantic Analysis Notes ──────────────────────────────────────────────
 * The semantic pass must enforce the following rules for trait declarations:
 * 1. **Trait Field Name Uniqueness**: All field names within a trait must be
 *    unique. Duplicate names with different types are a compile error.
 * 2. **Generic Parameters**: All generic parameters must be used in at least
 *    one field type. Unused parameters are a compile error.
 * 3. **No Trait Inheritance**: Traits do not inherit from other traits.
 * 4. **No Methods**: Traits define fields only – no methods, no behavior,
 *    no default values. All behavior is expressed as plain functions.
 */
struct TraitDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    ArenaSpan<GenericParamDeclPtr> genericParams;
    ArenaSpan<TraitFieldPtr> fields;

    TraitDeclAST() : TypeDeclAST(ASTKind::TraitDecl) {}
};
using TraitDeclPtr = TraitDeclAST*;

/**
 * @brief Represents a trait reference in a struct declaration or generic constraint.
 *
 * Grammar:
 *   trait_ref := IDENTIFIER
 *              | IDENTIFIER '<' type { ',' type } '>'
 *
 * @example
 *   : Vector2          → name="Vector2",    genericArgs={}
 *   : Named            → name="Named",      genericArgs={}
 *   : Container<int>   → name="Container",  genericArgs=[Int]
 *
 * The semantic pass resolves name to a TraitDeclAST and checks that
 * genericArgs count matches the trait's generic parameters.
 *
 * @note This is used in:
 *   - Struct declarations: `struct Entity : Vector2, Named { ... }`
 *   - Generic constraints: `<T : Vector2 + Named>`
 *   - Where clauses (future)
 */
struct TraitRefAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitRef;

    InternedString name;
    ArenaSpan<TypePtr> genericArgs;

    TraitRefAST() : BaseAST(ASTKind::TraitRef) {}
};
using TraitRefPtr = TraitRefAST*;

// ─────────────────────────────────────────────────────────────────────────────
// Aliases for common pointer types.
// ─────────────────────────────────────────────────────────────────────────────

using UseDeclPtr = UseDeclAST*;
using VarDeclPtr = VarDeclAST*;
using ParamPtr = ParamAST*;
using FuncDeclPtr = FuncDeclAST*;
using FieldDeclPtr = FieldDeclAST*;
using StructDeclPtr = StructDeclAST*;
using EnumVariantPtr = EnumVariantAST*;
using EnumDeclPtr = EnumDeclAST*;
using TraitFieldPtr = TraitFieldDeclAST*;
using TraitDeclPtr = TraitDeclAST*;
using TraitRefPtr = TraitRefAST*;