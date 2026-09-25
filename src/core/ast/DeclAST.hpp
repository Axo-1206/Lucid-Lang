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
/// ─── Removed Declarations ────────────────────────────────────────────────
/// This header deliberately has NO node for:
///   - `TraitDeclAST` as a pure field contract. Traits now carry both `FIELD`
///     and `REQUIRE` clauses (see `TraitDeclAST` and `TraitRequireDeclAST`).
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
///     - DEF declarations (DefDeclAST)
///     - Static members (StaticFnDeclAST)
/// 
///   TYPE NAMESPACE (TypeDeclAST):
///     - Structs (StructDeclAST)
///     - Enums (EnumDeclAST)
///     - Traits (TraitDeclAST)
///     - Host-backed types (HostTypeDeclAST)
///     - Type aliases (TypeAliasDeclAST)
/// 
/// This separation allows:
///   - `struct Point` and `let Point = 42` to coexist
///   - Faster lookup (search only the relevant namespace)
///   - Clearer error messages ("undefined variable" vs "undefined type")
/// 
/// ============================================================================
/// FIELD CATEGORIES
/// ============================================================================
///
/// | Category        | Mutability          | Set By  | Examples                          |
/// | --------------- | ------------------- | ------- | --------------------------------- |
/// | Parser Fields   | `const` (immutable) | Parser  | `name`, `type`, `init`, `body`    |
/// | Semantic Fields | `mutable`           | Sema    | `resolvedType`, `mangledName`,    |
/// |                 |                     |         | `fieldIndex`, `resourceKind`      |
///
/// ─── No CodeGen Fields ────────────────────────────────────────────────────
/// The AST holds NO LLVM-level facts. There is no `llvmType`, `llvmFunction`,
/// `llvmAlloca`, `totalSize`, `alignment`, or `byteSize` on any node. Every
/// LLVM-level fact lives on a codegen-side object:
///
///   - LLVM types:         `Types` (cached per `TypeAST*`)
///   - LLVM functions:     `ProgramState`'s function table
///   - Local storage:      `FunctionState`'s value map
///   - Sizes/alignments:   `DataLayout`, queried through `Types::sizeOf`
///                         / `Types::alignOf`
///
/// The reason is that LLVM-level facts are properties of one codegen run
/// against one target. The AST outlives any single `ProgramState` (the
/// interpreter lowers the same AST against new targets on hot reload), so
/// a cached LLVM fact on the AST would be a snapshot of the wrong run.

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <memory>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// ImportDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents an `import` declaration – imports a module by path.
/// 
/// @example
///   import core.io                → path = "core.io",   alias = "io" (derived)
///   import core.math as math      → path = "core.math", alias = "math"
///   import graphics.gl as gl      → path = "graphics.gl", alias = "gl"
/// 
/// The path is the module's identity. The alias is the local name used in
/// `alias::member` accesses. If no alias is written, the last path segment
/// is used as the alias.
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

// ─────────────────────────────────────────────────────────────────────────────
// VarDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a variable declaration with an explicit type annotation.
///
/// @example
///   let count int     = 0
///   const PI float    = 3.14159
///   let name string?  = nil
///
/// Type annotation is always required in Lucid – `type` is never null.
/// `init` is null when no initializer was written (valid for `let` only;
/// `const` must always have an initializer – enforced by the semantic pass).
///
/// ─── No Function Types ────────────────────────────────────────────────
/// A `VarDeclAST` never holds a `FuncTypeAST`. Function-typed bindings
/// are always `FuncDeclAST`. A variable declared `let f fn (int) -> int = ...`
/// is parsed as a `FuncDeclAST`, not a `VarDeclAST`; the two node kinds
/// are disjoint by declared type.
///
/// This is why every binding-classification site treats the "FuncDeclAST"
/// and "VarDeclAST" branches as mutually exclusive: a `VarDeclAST`'s type
/// is never a `FuncTypeAST`, so the function-typed branch of any classifier
/// fires only for `FuncDeclAST`.
///
/// Unlike a function type, a variable's type *may* be nullable or
/// fallible: `let x int? = nil`, `let y string! = ...`. Those wrap
/// non-function value types, never a function type.
struct VarDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ExprAST* init;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    InternedString mangledName;  // Mangled name for AOT compilation

    // ─── Constructor ─────────────────────────────────────────────────────
    VarDeclAST(InternedString n, DeclKeyword kw, TypeAST* t, ExprAST* i)
        : ValueDeclAST(ASTKind::VarDecl, n, kw, t)
        , init(i) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ParamAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a function parameter.
/// 
/// @example
///   In `const add (a int, b int) -> int`, `a` and `b` are ParamAST nodes.
/// 
/// Parameters are passed by value (a copy) by default. A `const` parameter
/// marks a read-only reference parameter – the function sees the caller's
/// original value but cannot modify it.
/// 
/// @field type          The parameter type (never null).
/// @field isVariadic    True if this is a variadic parameter (`...type`).
/// @field isConstParam  True if this is a read-only reference parameter (`const type`).
/// 
/// @note A variadic parameter must be the last parameter in its own param group.
///       Variadic parameters collect trailing arguments into a `[*]type` array.
struct ParamAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const bool isVariadic;        // True if variadic (`...type`)
    const bool isConstParam;      // True if read-only reference (`const type`)

    // ─── Constructor ─────────────────────────────────────────────────────
    ParamAST(InternedString n, TypeAST* t, bool variadic = false, bool isConstParam = false)
        : ValueDeclAST(ASTKind::Param, n, DeclKeyword::Let, t)
        , isVariadic(variadic)
        , isConstParam(isConstParam) {}
};
using ParamGroup = std::vector<ParamAST*>;

// ─────────────────────────────────────────────────────────────────────────────
// FuncDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A named function declaration — a binding whose value is a function.
///
/// ─── Design: Init Is an Expression, Not a Body ─────────────────────────
///
/// A `FuncDeclAST` is structurally similar to a `VarDeclAST`: it binds a
/// name to a value of a declared type. The differences are:
///
///   1. The declared type is always a `FuncTypeAST` (never nullable, never
///      fallible — `?`/`!` on function types is forbidden).
///   2. It carries `genericParams`.
///   3. Its initializer may be an `AnonFuncExprAST` (for a block body),
///      a reference expression (for a body that names another function),
///      or any other expression producing a value of the function type.
///
/// ─── Two `funcType` Fields, Only One Has Runtime Parameters ───────────
///
/// A `FuncDeclAST` has *two* `FuncTypeAST` nodes in play:
///
///   - `this->funcType` — the *declared* signature, parsed from the
///     declaration header. Used for type comparison at call sites, generic
///     substitution, and diagnostics. Its `ParamAST` nodes are type-only:
///     they are never allocated, never bound, and never registered as
///     bindings.
///
///   - `init->as<AnonFuncExprAST>()->funcType` — the *runtime* signature,
///     parsed from the block body. Its `ParamAST` nodes are the real
///     parameters. CodeGen iterates *this* field to allocate each
///     parameter's stack slot and register it as a binding.
///
/// The parser produces both, and the two signatures match by construction.
/// CodeGen must read parameters from `init` (via the `AnonFuncExprAST`),
/// never from `this->funcType`.
///
/// When `init` is a reference expression (an `IdentifierExprAST`,
/// `ModuleAccessExprAST`, call) rather than an `AnonFuncExprAST`, there are
/// no runtime parameters for *this* declaration — the reference target's own
/// `AnonFuncExprAST` carries them, and this declaration is a pure alias.
///
/// ─── Reassignment ──────────────────────────────────────────────────────
/// `f = expr;` replaces `init` with the new expression. It follows the
/// ordinary `assign_stmt` rules: `f` must be `let`, and the expression
/// must evaluate to a value assignable to the declared `funcType`.
///
/// ─── Invariant: Generic ⇒ `const` ───────────────────────────────────────
/// `!genericParams.empty() ⇒ keyword == DeclKeyword::Const`. The parser
/// accepts either keyword here (`func_decl` still reads `('let' | 'const')`),
/// so this is a Sema-enforced invariant, not a parse-time one — Sema rejects
/// `let` on a generic declaration with a targeted diagnostic (D1: "a generic
/// function must be declared 'const'").
///
/// The reasoning: a generic function's declared type is a *family*, not a
/// value. No expression form in the language ever produces a family, so a
/// `let`-bound generic `FuncDeclAST` would have no legal `init` expression
/// that could ever reassign it. `const` is simply what the declaration
/// already is.
///
/// ─── Async ──────────────────────────────────────────────────────────────
/// `isAsync` is set when the declaration is marked `async`. Only `async`
/// functions may be `spawn`ed, `start`ed, or `await`ed. A bare call to an
/// `async` function (without `spawn`, `start`, or `await start`) is a
/// compile error.
struct FuncDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    // Invariant: !genericParams.empty() ⇒ keyword == DeclKeyword::Const.
    ArenaSpan<GenericParamDeclAST*> genericParams;

    /// The declared function type — parsed from the declaration header.
    /// Remains fixed across reassignment; only `init` changes.
    FuncTypeAST* funcType = nullptr;

    /// The initializer — an expression producing a value of `funcType`.
    ///
    ///   - Block-body declaration: an `AnonFuncExprAST` wrapping the block,
    ///     its `funcType` set from this declaration's `funcType`.
    ///   - Reference body: the reference expression itself
    ///     (`IdentifierExprAST`, `ModuleAccessExprAST`, `CallExprAST`, ...).
    ///   - Reassignment: replaced with the new expression.
    ///   - Foreign body: `init == nullptr`, and `isForeignFunction == true`.
    ExprAST* init = nullptr;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    bool isForeignFunction = false;   // @[foreign("C")]
    bool isInline = false;            // @[inline]
    bool isNoInline = false;          // @[noinline]
    bool isAsync = false;             // `async` marker

    InternedString mangledName;

    // ─── Constructor ────────────────────────────────────────────────────
    FuncDeclAST(InternedString n, DeclKeyword kw,
                ArenaSpan<GenericParamDeclAST*> params,
                FuncTypeAST* ft, ExprAST* i)
        : ValueDeclAST(ASTKind::FuncDecl, n, kw, ft)
        , genericParams(params)
        , funcType(ft)
        , init(i) {}

    /// True if this declaration has type parameters. Per the invariant
    /// above, `isGeneric() == true` implies `keyword == DeclKeyword::Const`.
    bool isGeneric() const { return !genericParams.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// EnumVariantAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents one variant of an enum — either integer-valued or payload-carrying.
/// 
/// Two forms:
///   - Integer form:  `North = 0`  — `hasValue == true`, `value` holds the integer.
///   - Payload form:  `Num(float)` — `hasValue == false`, `payloadType` holds the inner type.
/// 
/// The semantic pass computes tag indices for both forms and verifies no
/// duplicate values (for integer-form enums). A payload variant's tag is its
/// position in the enum's declaration order, starting from 0.
/// 
/// @example
///   enum Direction { North = 0; East = 1; }
///   enum JsonValue { Num(float); Str(string); }
///   enum Token { Eof = 0; Ident(string); Number(float); }
/// 
/// @note Enum variants are accessed as `Direction.North` or `JsonValue.Num(x)`
///       in source. They live in the value namespace of the enum's scope.
struct EnumVariantAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumVariant;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    const bool    hasValue;      // true = integer form (`Variant = N`)
    const int64_t value;         // valid iff hasValue; the explicit integer value
    TypeAST*      payloadType;   // valid iff !hasValue; the payload type

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    size_t tagIndex = SIZE_MAX;  // the variant's discriminant; assigned by Sema

    // ─── Integer form constructor ────────────────────────────────────────
    EnumVariantAST(InternedString n, int64_t v)
        : ValueDeclAST(ASTKind::EnumVariant, n, DeclKeyword::Const, nullptr)
        , hasValue(true), value(v), payloadType(nullptr) {}

    // ─── Payload form constructor ────────────────────────────────────────
    EnumVariantAST(InternedString n, TypeAST* t)
        : ValueDeclAST(ASTKind::EnumVariant, n, DeclKeyword::Const, nullptr)
        , hasValue(false), value(0), payloadType(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A struct field — a typed slot, optionally with a default value.
///
/// ─── Design: Same Shape as FuncDeclAST, for the Same Reason ────────────
///
/// A struct field is not a declaration — it's a *slot* in the struct's
/// layout. It has no generic parameters of its own. When a field's type is
/// a function type and the field supplies a block-body default, that block
/// is parsed into an `AnonFuncExprAST` (with a synthesized `self` parameter
/// prepended by the parser) and stored as `defaultVal`. This is the same
/// pattern as `FuncDeclAST`: the block body becomes an expression, and
/// everything downstream treats it uniformly.
///
/// ─── @[opaque] ──────────────────────────────────────────────────────────
/// A field marked `@[opaque]` cannot be initialized, read, or assigned from
/// Lucid source. It is skipped by `toStr`'s struct walker and cannot satisfy
/// a trait's `FIELD` clause of the same name. It still occupies layout space.
/// A struct with only `@[opaque]` fields has no literal form; it must be
/// constructed by an `FN` in the core script (e.g., `map_new`).
/// The `isOpaque` flag is set by Sema during attribute resolution.
struct FieldDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    /// The field's initializer, if any.
    ///
    ///   - Non-function field: any expression producing a value of `type`.
    ///   - Function field, block-body default: an `AnonFuncExprAST` whose
    ///     `funcType` is `type` with a synthesized `self: &StructName`
    ///     parameter prepended.
    ///   - Function field, expression default: any expression producing
    ///     a value of `type`.
    ///   - No default: `nullptr`.
    ExprAST* defaultVal = nullptr;

    const bool isConstField;   // `const` modifier on the field

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    size_t fieldIndex = 0;     // position in struct layout
    bool   isOpaque = false;   // true when @[opaque] is present

    // ─── Constructor ────────────────────────────────────────────────────
    FieldDeclAST(InternedString n, TypeAST* t, ExprAST* dv, bool isConstField)
        : ValueDeclAST(ASTKind::FieldDecl, n, DeclKeyword::Let, t)
        , defaultVal(dv)
        , isConstField(isConstField) {}

    bool isConst() const { return isConstField; }
};

// ─────────────────────────────────────────────────────────────────────────────
// StructDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a struct definition with fields and optional generic parameters.
///
/// @example
///   struct Point { x float = 0.0; y float = 0.0 }
///   struct Node<T> { value T; next Node<T>?; }
///   struct Entity : Vector2, Named { name string; x float; y float; health int; }
///
/// A struct may list traits it satisfies after `:`. The traits are stored in
/// `traitRefs` and resolved during semantic analysis.
///
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// The semantic pass enforces:
/// 1. **Trait conformance**: For each trait in `traitRefs`, the struct must
///    declare all fields the trait requires (for `FIELD` clauses) and provide
///    the required operations (for `REQUIRE` clauses, via `DEF`s).
/// 2. **Const matching**: A trait field marked `const` requires the struct's
///    corresponding field to also be `const`.
/// 3. **Type matching**: All trait fields must have matching types.
/// 4. **Generic parameters**: All generic parameters must be used in at least
///    one field type. Unused parameters are a compile error.
/// 5. **Recursive fields**: A field whose type recursively contains the
///    enclosing struct must be nullable (`?`). Two cases:
///      - **Value field** (`next Node`): infinite size. The `?` in
///        `next Node?` makes the field a nullable pointer, breaking the
///        recursion.
///      - **Reference field** (`next &Node`): the field is a pointer, so
///        there is no infinite-size problem — but a non-nullable recursive
///        reference cannot be initialized (every instance would need
///        another instance to point at, with no terminating case). The `?`
///        in `next &Node?` provides a `nil` case that terminates the
///        chain.
///    Both cases are fixed by `?`.
///
/// ─── No Layout Fields ───────────────────────────────────────────────────
/// This node has NO `totalSize` or `alignment` fields. Size and alignment are
/// `DataLayout`-dependent — a property of the target, not the AST. CodeGen
/// queries them on demand:
///
///     llvm::Type* ty = program.types().get(someStructTypeAST);
///     uint64_t size  = module.getDataLayout().getTypeAllocSize(ty);
///     uint64_t align = module.getDataLayout().getABITypeAlign(ty);
struct StructDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    ArenaSpan<FieldDeclAST*>        fields;
    ArenaSpan<NamedTypeAST*>        traitRefs;
    const bool isPacked = false;  // From @[packed] attribute

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// The linker-level name of the struct's LLVM type. Read by
    /// `Types::structType` to name the `llvm::StructType`.
    InternedString mangledName;

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

    bool isGeneric() const { return !genericParams.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// EnumDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents an enum definition.
///
/// @example
///   enum Direction { North = 0; East = 1; South = 2; West = 3; }
///   enum Status : int32 { Ok = 200; NotFound = 404; Error = 500; }
///   enum JsonValue { Num(float); Str(string); Arr([*]JsonValue); }
///
/// ─── Two Kinds of Enum ──────────────────────────────────────────────────
/// An enum is integer-valued if every variant uses the `Variant = N` form.
/// It lowers to a bare integer of the backing type.
///
/// An enum is payload-carrying if any variant uses the `Variant(T)` form.
/// It lowers to `{ tag: int, payload: byte[] }`, where `tag` identifies
/// which variant is active and `payload` is sized to hold the largest
/// variant. The backing type names the **tag's** integer type; the payload
/// is sized independently.
///
/// Mixed enums (integer and payload variants in one declaration) are legal.
/// An enum with any payload variant is a payload enum for all purposes.
///
/// ─── No Layout Fields ───────────────────────────────────────────────────
/// Like `StructDeclAST`, this node has NO `backingLLVMType` or `byteSize`.
/// The backing LLVM integer type is derived on demand by `Types::enumType`;
/// the size is a `DataLayout` query.
///
/// @field variants         The enum's variants, in declaration order.
/// @field backingType      Optional backing integer type. For integer enums,
///                         this is the enum's storage; for payload enums,
///                         this is the tag's integer type. Defaults to `int`
///                         for integer enums and the smallest unsigned type
///                         that holds the variant count for payload enums.
/// @field isPayloadEnum    True if any variant carries a payload. Computed
///                         by Sema during enum resolution.
/// @field isIntegerEnum    True if all variants are integer-valued. Computed
///                         by Sema during enum resolution.
struct EnumDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<EnumVariantAST*> variants;
    PrimitiveTypeAST*          backingType;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    InternedString mangledName;
    bool isPayloadEnum = false;
    bool isIntegerEnum = false;

    // ─── Constructor ─────────────────────────────────────────────────────
    EnumDeclAST(InternedString n,
                ArenaSpan<EnumVariantAST*> vars,
                PrimitiveTypeAST* backing = nullptr)
        : TypeDeclAST(ASTKind::EnumDecl, n)
        , variants(vars)
        , backingType(backing) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TraitFieldDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a single `FIELD` clause inside a trait — a field requirement.
/// 
/// A `FIELD` clause declares a field (name, type, optional const-ness) that
/// any type satisfying the trait must declare with matching name and type.
/// Traits never carry default values; a `FIELD` clause is a requirement, not
/// a contribution.
/// 
/// @example
///   trait Vector2 { FIELD x float; FIELD y float; }
///   trait Named { FIELD name string; }
///   trait ImmutableConfig { FIELD const maxRetries int; FIELD const timeout float; }
/// 
/// ─── Trait Field Rules ──────────────────────────────────────────────────
/// 1. **Name, type, const-ness only**: No default values.
/// 2. **Const requirement**: If `isConstField` is true, the implementing
///    struct must declare the field as `const`.
/// 3. **Type restrictions**: If `isConstField` is true, the field type must
///    be definite (not nullable or fallible). If false, the field type may
///    be nullable, fallible, or combined.
/// 4. **Self-reference**: A `FIELD next Self?;` clause is legal and means
///    "a nullable reference to the concrete type being satisfied." A
///    non-nullable `FIELD next Self;` is a compile error (infinite size).
///    See `TraitDeclAST` for the full self-reference rule.
/// 
/// @note Not a `ValueDeclAST` because trait fields are requirements, not
///       actual values. The semantic pass uses them to verify that satisfying
///       types declare the required fields.
struct TraitFieldDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitFieldDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    TypeAST* type;            // Required field type
    const bool isConstField;  // True if the implementing type must declare as const

    // ─── Constructor ─────────────────────────────────────────────────────
    TraitFieldDeclAST(InternedString n, TypeAST* t, bool isConstField)
        : DeclAST(ASTKind::TraitFieldDecl, n)
        , type(t)
        , isConstField(isConstField) {}

    bool isConst() const { return isConstField; }
};

// ─────────────────────────────────────────────────────────────────────────────
// TraitRequireDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a single `REQUIRE` clause inside a trait — an operation requirement.
///
/// A `REQUIRE` clause declares that any type satisfying the trait must
/// provide an operation of a given kind and signature. The operation kind is
/// named by an `OpKind` value (e.g., `BINARY_OP`, `CALL`), and the symbol is
/// the operation's name (`+`, `toStr`, ...).
///
/// @example
///   trait Numeric {
///       REQUIRE BINARY_OP '+' (self Self, rhs Self) -> Self;
///       REQUIRE BINARY_OP '-' (self Self, rhs Self) -> Self;
///   }
///   trait Stringable {
///       REQUIRE CALL 'toStr' (self Self) -> string;
///   }
///
/// ─── Semantic Rules ─────────────────────────────────────────────────────
/// 1. **Only inside a trait**: A `REQUIRE` clause appears only in a `trait`
///    body. Anywhere else is a parse error.
/// 2. **No body**: A `REQUIRE` clause is a shape, not an implementation.
/// 3. **Resolution**: `opKindName` is resolved by Sema against `OpKind`
///    values declared in the core script. An unknown name is a compile
///    error.
///
/// @field opKindName   The operation category identifier (`"BINARY_OP"`, `"CALL"`, ...).
/// @field symbol       The operation's string symbol (`"+"`, `"toStr"`, ...).
/// @field params       The required operation's parameter list.
/// @field returnType   The required operation's return type.
struct TraitRequireDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitRequireDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    InternedString  opKindName;
    InternedString  symbol;
    ArenaSpan<ParamAST*> params;
    TypeAST*        returnType = nullptr;

    TraitRequireDeclAST(InternedString opk, InternedString sym)
        : DeclAST(ASTKind::TraitRequireDecl, InternedString())
        , opKindName(opk), symbol(sym) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TraitDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a trait — a named set of requirements that a type
///        promises to satisfy.
/// 
/// @example
///   trait Vector2 { FIELD x float; FIELD y float; }
///   trait Numeric {
///       REQUIRE BINARY_OP '+' (self Self, rhs Self) -> Self;
///       REQUIRE BINARY_OP '-' (self Self, rhs Self) -> Self;
///   }
///   trait Ord : Eq {
///       REQUIRE BINARY_OP '<' (self Self, rhs Self) -> bool;
///   }
/// 
/// ─── One Construct, Two Clause Kinds ────────────────────────────────────
/// A trait body may contain:
///   - `FIELD name type;` clauses — field requirements.
///   - `REQUIRE op_kind "symbol" (params) -> type;` clauses — operation
///     requirements.
///
/// A trait with only `FIELD` clauses is struct-flavored: only types with
/// fields can satisfy it. A trait with only `REQUIRE` clauses is
/// operation-flavored: any type that provides the operations can satisfy
/// it, including primitives. A trait may have both kinds.
/// 
/// ─── Trait Inheritance ──────────────────────────────────────────────────
/// `trait X : A, B { ... }` declares that any type satisfying `X` must also
/// satisfy `A` and `B`. Parent traits are checked first; a type that fails
/// a parent cannot satisfy the child. The parents are checked in
/// declaration order.
/// 
/// ─── Generic Traits ─────────────────────────────────────────────────────
/// Traits may carry generic parameters. A constraint site uses a concrete
/// instantiation: `<T : Container<int>>`. The compiler substitutes the
/// arguments at the constraint site.
/// 
/// ─── Self-Reference Rules ───────────────────────────────────────────────
/// `Self` in a clause refers to the concrete type being satisfied. A
/// `FIELD` clause may use `Self` in the field's type:
///   - `FIELD next Self?;` — a nullable reference to the concrete type.
///     Legal; the `?` provides the terminating case.
///   - `FIELD next Self;` — a non-nullable self-reference. **Compile
///     error**: infinite size.
/// 
/// ─── Semantic Analysis Notes ────────────────────────────────────────────
/// 1. **Field name uniqueness**: All `FIELD` names within a trait must be
///    unique. Duplicate names with different types are a compile error.
/// 2. **Generic parameters**: All generic parameters must be used in at least
///    one clause.
/// 3. **No default values**: Trait clauses declare requirements only.
struct TraitDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    ArenaSpan<NamedTypeAST*>        parentTraits;  // `trait X : A, B { ... }`
    ArenaSpan<TraitFieldDeclAST*>   fields;        // FIELD clauses
    ArenaSpan<TraitRequireDeclAST*> requires;      // REQUIRE clauses

    // ─── Constructor ─────────────────────────────────────────────────────
    TraitDeclAST(InternedString n,
                 ArenaSpan<GenericParamDeclAST*> params,
                 ArenaSpan<NamedTypeAST*> parents,
                 ArenaSpan<TraitFieldDeclAST*> flds,
                 ArenaSpan<TraitRequireDeclAST*> reqs)
        : TypeDeclAST(ASTKind::TraitDecl, n)
        , genericParams(params)
        , parentTraits(parents)
        , fields(flds)
        , requires(reqs) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SatisfyDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a `satisfy` block — asserts that a type satisfies a trait.
///
/// A `satisfy` block declares that `targetType` implements `traitName` by
/// supplying `DEF` declarations that fulfil the trait's `REQUIRE` clauses.
/// Field conformance (for `FIELD` clauses) is checked implicitly against the/// target type's own field declarations; the block does not list fields.
///
/// @example
///   satisfy Numeric for Vec2 {
///       DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = { ... };
///       DEF BINARY_OP '-' (a Vec2, b Vec2) -> Vec2 = { ... };
///   }
///
///   satisfy Vector2 for Vec2 { }    -- empty block: fields checked implicitly
///
/// ─── Generic Satisfy ────────────────────────────────────────────────────
/// A `satisfy` block may be generic, covering all instantiations of a
/// generic trait in one block:
///
///   satisfy Container<T> for Box<T> { }
///
/// The parameter list `<T>` is shared between the trait application and the
/// type. Every parameter that appears on either side must be listed.
///
/// ─── Semantic Rules ─────────────────────────────────────────────────────
/// 1. **Missing member** (a `REQUIRE` clause with no matching `DEF`): compile
///    error.
/// 2. **Extra `DEF`** (a `DEF` in the block that the trait does not require):
///    allowed. The block is both an assertion of conformance and a grouping
///    of related declarations.
/// 3. **Field conformance**: checked against the target type's own fields,
///    not listed in the block.
///
/// @field traitName          The trait being satisfied.
/// @field traitGenericArgs   Generic arguments instantiating the trait, if any.
/// @field genericParams      For generic satisfy: `satisfy Container<T> for Box<T>`.
/// @field targetType         The `for Type` clause.
/// @field defs               The `DEF` declarations fulfilling the trait's requirements.
struct SatisfyDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::SatisfyDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    InternedString  traitName;
    ArenaSpan<TypeAST*> traitGenericArgs;
    ArenaSpan<GenericParamDeclAST*> genericParams;
    TypeAST*        targetType = nullptr;
    ArenaSpan<DefDeclAST*> defs;

    SatisfyDeclAST(InternedString tn)
        : DeclAST(ASTKind::SatisfyDecl, InternedString()), traitName(tn) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// DefDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a `DEF` declaration — an operator or named-call fact.
///
/// `DEF` declares that an operation exists for a given signature. It appears
/// at the top level of a module or inside a `satisfy` block. The declaration
/// names the operation kind, the symbol, the parameter list, the return
/// type, and the implementation.
///
/// @example
///   DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = { ... };
///   DEF CALL 'toStr' (v Vec2) -> string = #builtin(vec2_to_str);
///   DEF BINARY_OP '!=' <T : Eq> (a T, b T) -> bool = { return not (a == b); };
///
/// ─── Implementation Forms ───────────────────────────────────────────────
/// The `impl` expression is one of:
///   - An `AnonFuncExprAST` whose signature is taken from the `DEF` header
///     (a block body — the most common form).
///   - An `IdentifierExprAST` referencing an existing function.
///   - A `CallExprAST` to a `#host`, `#native`, or `#builtin` target.
///
/// The block form's signature is not repeated; the parser synthesizes the
/// anonymous function's signature from `params` and `returnType`.
///
/// ─── Semantic Rules ─────────────────────────────────────────────────────
/// 1. **Op-kind resolution**: `opKindName` resolves to an `OpKind` value at
///    the DEF's declaration site. An unknown name is a compile error.
/// 2. **Overload resolution**: Two `DEF`s with the same op kind, symbol, and
///    operand types are a redeclaration error. Exact concrete matches beat
///    generic matches; two matching generics are an ambiguity error.
///
/// @field opKindName   The operation category (`"BINARY_OP"`, `"CALL"`, ...).
/// @field symbol       The operation's string symbol (`"+"`, `"toStr"`, ...).
/// @field genericParams   Generic parameters for a generic DEF.
/// @field params       The implementation's parameters.
/// @field returnType   The implementation's return type.
/// @field impl         The implementation — an `AnonFuncExprAST`, an
///                     `IdentifierExprAST`, or a call to a host/native/builtin.
struct DefDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::DefDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    InternedString  opKindName;
    InternedString  symbol;
    ArenaSpan<GenericParamDeclAST*> genericParams;
    ArenaSpan<ParamAST*> params;
    TypeAST*        returnType = nullptr;
    ExprAST*        impl = nullptr;

    DefDeclAST(InternedString opk, InternedString sym)
        : DeclAST(ASTKind::DefDecl, InternedString())
        , opKindName(opk), symbol(sym) {}
};



// ─────────────────────────────────────────────────────────────────────────────
// RecognizedHostKind — the small set of host-backed types whose behavior
// the compiler hardcodes.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Recognizes a host-backed type whose ownership or linearity
///        behavior the compiler treats specially.
///
/// A `HostTypeDeclAST` whose `recognizedKind` is not `None` is one of the
/// language's own runtime-backed types — `Deferred<T>` — declared by the
/// core script. The compiler uses this field to decide, at each use site,
/// whether the type participates in a special ownership or linearity rule.
///
/// ─── This is not a user extension point ───────────────────────────────────
/// A user's custom `#host` type is always `None`. Its ownership behavior
/// is expressed through the `#host` operations the user registers for it
/// (copy, drop, and whatever else the type supports), not by adding to
/// this enum. Adding a value here means adding compiler behavior — a new
/// `ResourceKind` case, a new set of language rules — which is a change
/// to the language, not a change to a host engine.
///
/// ─── Why Arena is not here ────────────────────────────────────────────────
/// `Arena` was a boot-level type in an earlier draft. Under the current
/// grammar it is not: the language has no raw pointers, and an arena's
/// purpose was to hand out raw addresses into a caller-managed block.
/// Without `*T`, an arena has no way to expose what it allocates. The
/// type is gone, and with it the `ResourceKind::Arena` case.
///
/// If a memory-chunk type is wanted later, it can be designed against the
/// current model (a host-backed `NamedTypeAST` with `#host` operations)
/// and classified as `OwnedBuffer` or `None` depending on how its
/// operations manage memory. That is a future language feature, not a
/// slot in this enum.
enum class RecognizedHostKind : uint8_t {
    None,       ///< An ordinary host-backed type. Copy and drop follow
                ///< the user's registered operations; the compiler
                ///< treats it as any other value type.

    Deferred,   ///< The core script's `Deferred<T>`. Linear: must be
                ///< consumed by `await` or `cancel`; classified as
                ///< `ResourceKind::Handle`; rejected from aggregates.
};

// ─────────────────────────────────────────────────────────────────────────────
// HostTypeDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A host-backed type declaration — `TYPE X = #host(...)` /
///        `#native(...)` / `#builtin(...)`.
///
/// Binds a Lucid type name to a type whose implementation lives on the host
/// (a C++ type registered with the engine), in the VM (`#native`), or in the
/// compiler (`#builtin`). Host-backed types may be generic.
///
/// @example
///   TYPE Map<K, V> = #host(LucidMap)
///   TYPE Deferred<T> = #host(LucidDeferred)
///   TYPE Weak<T> = #host(LucidWeak)
///   TYPE Simd<T, N> = #builtin(simd_type)
///   TYPE Float4 = Simd<float, 4>       -- this is a TypeAliasDeclAST, not a host decl
///
/// ─── Semantic Rules ─────────────────────────────────────────────────────
/// 1. **Core-script only for `#native`**: A user script using `#native` is a
///    compile error.
/// 2. **Target name resolution**: `targetName` is resolved by the compiler
///    against the host registry (`#host`), VM opcode table (`#native`), or
///    builtin registry (`#builtin`). An unknown name is a compile error.
///
/// @field genericParams   Generic parameters (e.g., `T`, `N`).
/// @field kind            Whether this is `#host`, `#native`, or `#builtin`.
/// @field targetName      The identifier inside `#host(...)`, `#native(...)`,
///                        or `#builtin(...)`.
enum class HostTypeKind { Host, Native, Builtin };

struct HostTypeDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::HostTypeDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    HostTypeKind    kind;
    InternedString  targetName;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// @brief Set by Sema when it resolves this declaration as one of the
    ///        core script's runtime-backed types.
    ///
    /// Only the core script's `Deferred<T>` gets a non-`None` value. Every
    /// user-declared `#host` type — and every other core-script host type
    /// (`Map`, `Weak`, `OpKind`, the primitives) — is `None`.
    ///
    /// The classifier `classifyResourceKind` reads this field to decide
    /// whether the type participates in the language's ownership model
    /// (`Deferred` → `ResourceKind::Handle`) or is an ordinary host type
    /// whose resource behavior is whatever the user's `#host` operations
    /// implement.
    RecognizedHostKind recognizedKind = RecognizedHostKind::None;

    HostTypeDeclAST(InternedString n, HostTypeKind k, InternedString t)
        : TypeDeclAST(ASTKind::HostTypeDecl, n)
        , kind(k), targetName(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeAliasDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A type alias — `TYPE X = Y`.
///
/// Binds a new name `X` to an existing type expression `Y`. The alias may
/// be generic.
///
/// @example
///   TYPE Int32 = int
///   TYPE StringMap<V> = Map<string, V>
///   TYPE PlayerList = [*]Player
///
/// ─── Alias Transparency ─────────────────────────────────────────────────
/// An alias is transparent: a value of type `Int32` is a value of type `int`,
/// and vice versa. There is no wrapper, no conversion, and no distinct
/// identity. Alias chains are followed until a terminal target is reached
/// (a struct, enum, host type, or primitive). A cycle in the alias chain is
/// a compile error.
///
/// @field genericParams   Generic parameters for a generic alias.
/// @field targetType      The RHS type expression.
struct TypeAliasDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeAliasDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<GenericParamDeclAST*> genericParams;
    TypeAST* targetType = nullptr;

    TypeAliasDeclAST(InternedString n, TypeAST* t)
        : TypeDeclAST(ASTKind::TypeAliasDecl, n), targetType(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// StaticFnDeclAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A static member function declared inside a struct body.
///
/// Static functions belong to the struct type, not to any instance. They are
/// called via `::`: `Vec2::zero()`. They have no implicit `self` parameter
/// and no per-instance storage.
///
/// @example
///   struct Vec2 {
///       x float = 0.0;
///       y float = 0.0;
///       static zero () -> Vec2 { Vec2 { x = 0.0, y = 0.0 } }
///       static fromAngle (theta float) -> Vec2 { ... }
///   }
///
///   const origin Vec2 = Vec2::zero()
///
/// ─── Semantic Rules ─────────────────────────────────────────────────────
/// 1. **No receiver**: A static function has no implicit `self`. It cannot
///    reference the struct's fields without an explicit parameter.
/// 2. **Namespaced**: The function's name is scoped to the struct. It is
///    accessed as `StructName::functionName`.
/// 3. **No generics**: A static function does not carry its own generic
///    parameters; it may use the struct's generic parameters.
///
/// @field params       The function's parameters (no implicit self).
/// @field returnType   The return type.
/// @field body         The implementation — an `AnonFuncExprAST` for a block
///                     body, or any expression producing a value of the
///                     function type.
struct StaticFnDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::StaticFnDecl;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    ArenaSpan<ParamAST*> params;
    TypeAST* returnType = nullptr;
    ExprAST* body = nullptr;

    StaticFnDeclAST(InternedString n)
        : DeclAST(ASTKind::StaticFnDecl, n) {}
};