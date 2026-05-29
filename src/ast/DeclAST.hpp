/**
 * @file DeclAST.hpp
 *
 * @responsibility Defines AST nodes for declarations – entities that introduce
 *                 new names into a scope (functions, structs, variables, etc.).
 *
 * @hierarchy BaseAST → DeclAST → [Concrete Decl Nodes]
 *
 * @related_files
 *   - src/parser/ParserDecl.cpp – primary producer of these nodes
 *   - src/semantic/SemanticCollector.cpp – consumes for symbol table population
 *   - src/semantic/TypeResolver.cpp – resolves types and generic parameters
 *
 * @note Doc comments and attributes are stored in the DeclAST base class,
 *       not in every BaseAST node. This matches the grammar and reduces
 *       memory footprint for non‑declaration nodes.
 *
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <memory>
#include <optional>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// Visibility – three visibility tiers for declarations.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Controls the visibility of a declaration across file/package boundaries.
 *
 * - Private: visible only within the current file (default).
 * - Package: visible to the entire package (module).
 * - Export:  visible to external consumers (public API).
 */
enum class Visibility {
    Private,
    Package,
    Export
};

// ─────────────────────────────────────────────────────────────────────────────
// DeclKeyword – declaration keyword for variables and functions.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Distinguishes `let` (mutable) from `const` (immutable) declarations.
 *
 * Stored on VarDeclAST and FuncDeclAST so the semantic pass can enforce
 * reassignability and nil rules without inspecting token types.
 */
enum class DeclKeyword {
    Let,   // reassignable, mutable in place, nil allowed
    Const, // not reassignable, not mutable in place, nil allowed
};

// ─────────────────────────────────────────────────────────────────────────────
// PackageDeclAST – the `package` declaration at the start of every file.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents `package name` – the first non‑comment line of every .luc file.
 *
 * Exactly one per file; the parser enforces this. ProgramAST also stores the
 * package name, but this node provides source location for error reporting
 * when the name mismatches the directory structure.
 *
 * @example
 *   package math
 */
struct PackageDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::PackageDecl;

    InternedString name;   // "math", "renderer", "app", etc.

    explicit PackageDeclAST(InternedString n)
        : DeclAST(ASTKind::PackageDecl), name(n) {}
};
using PackageDeclPtr = ASTPtr<PackageDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// UseDeclAST – imports a module path with optional alias.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a `use` declaration – imports symbols from another module.
 *
 * @example
 *   use math.vec2           → path = ["math","vec2"], alias = nullopt
 *   use math as m           → path = ["math"],        alias = "m"
 *   use renderer.types      → path = ["renderer","types"]
 *
 * Path segments are split on '.'. The semantic pass joins them back when
 * resolving against the package root. The alias, if present, becomes the
 * name by which the imported module is referenced.
 */
struct UseDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UseDecl;

    ArenaSpan<InternedString> path;              // e.g., ["math", "vec2"]
    std::optional<InternedString> alias;         // present when `as IDENT` was written
    Visibility visibility = Visibility::Private;

    UseDeclAST() : DeclAST(ASTKind::UseDecl) {}
};
using UseDeclPtr = ASTPtr<UseDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// VarDeclAST – variable declaration (let or const).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a variable declaration with an explicit type annotation.
 *
 * @example
 *   let   count int     = 0
 *   const PI    float   = 3.14159
 *   let   name  string? = nil
 *
 * Type annotation is always required in Luc – `type` is never null.
 * `init` is null when no initialiser was written (valid for `let` only;
 * `const` must always have an initialiser – enforced by semantic pass).
 */
struct VarDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    DeclKeyword keyword;           // Let or Const
    InternedString name;
    TypePtr type;                  // always present
    ExprPtr init;                  // nullptr if no initialiser
    Visibility visibility = Visibility::Private;

    VarDeclAST() : DeclAST(ASTKind::VarDecl) {}
};
using VarDeclPtr = ASTPtr<VarDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FuncDeclAST – function declaration (top‑level or local).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a function declaration.
 *
 * @example
 *   let add (a int) (b int) int = { return a + b }
 *   let fetch ~async (url string) -> string = { return await httpGet(url) }
 *   @extern("printf") const printf (fmt *uint8, args ...any) -> int
 *
 * The function type (parameter groups, return types, qualifiers) is stored in
 * `funcType`. The body is always a BlockStmtAST (the parser desugars expression
 * bodies into `return expr`).
 *
 * Visibility is only meaningful at top‑level; inside blocks, the parser forces
 * Private. Attributes (e.g., @extern, @inline) are stored in DeclAST::attributes.
 */
struct FuncDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    DeclKeyword keyword;
    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;   // empty if non‑generic
    FuncTypeAST funcType;                       // full function type (includes qualifiers)
    StmtPtr body;                               // always BlockStmtAST
    Visibility visibility = Visibility::Private;

    // Convenience accessors
    bool isAsync()   const { return funcType.isAsync(); }
    bool hasParams() const { return funcType.sig.hasParams(); }

    FuncDeclAST() : DeclAST(ASTKind::FuncDecl) {}
};
using FuncDeclPtr = ASTPtr<FuncDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FieldDeclAST – a single field inside a struct body.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a struct field, optionally with a default value.
 *
 * @example
 *   x     float           → defaultVal = nullptr
 *   r     float = 1.0     → defaultVal = literal 1.0
 *   items [*]string       → type = DynamicArray(String)
 *
 * The semantic pass enforces that struct literals must supply every field
 * that lacks a default value.
 */
struct FieldDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    InternedString name;
    TypePtr type;
    ExprPtr defaultVal;   // nullptr if no default was written

    FieldDeclAST() : BaseAST(ASTKind::FieldDecl) {}
};

using FieldDeclPtr = ASTPtr<FieldDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// StructDeclAST – a struct type declaration.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a struct definition with fields and optional generic parameters.
 *
 * @example
 *   struct Vec2 { x float, y float }
 *   struct Scene<T : Drawable> { objects []T }
 *
 * Impl blocks are separate top‑level declarations (ImplDeclAST). The struct
 * itself only holds its fields and generic params.
 *
 * ## Self‑Type (Semantic Cache)
 *
 * During semantic analysis, each struct needs a NamedTypeAST representing
 * itself as a type (e.g., "Point" as a type). This is stored in `selfType`
 * and initialized lazily by SemanticCollector.
 *
 * Why mutable and ASTPtr:
 *   - `mutable`: allows lazy initialisation from const contexts (visitors)
 *   - `ASTPtr`: provides stable ownership; the symbol table stores a raw
 *               pointer to selfType.get(), valid for the struct's lifetime
 */
struct StructDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;   // empty if non‑generic
    ArenaSpan<FieldDeclPtr> fields;
    Visibility visibility = Visibility::Private;

    // Semantic cache: self‑type representing this struct as a type.
    // Initialized lazily by SemanticCollector.
    mutable ASTPtr<NamedTypeAST> selfType;

    StructDeclAST() : DeclAST(ASTKind::StructDecl) {}
};
using StructDeclPtr = ASTPtr<StructDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// EnumVariantAST – a single named constant inside an enum.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents one variant of an enum, optionally with an explicit value.
 *
 * @example
 *   North         → explicitValue = nullopt (auto‑assigned)
 *   Vertex = 0x01 → explicitValue = 1
 *
 * The semantic pass computes final integer values:
 *   - Auto variants start at 0, increment by 1
 *   - Explicit values reset the counter
 * Duplicate values within the same enum are a semantic error.
 */
struct EnumVariantAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::EnumVariant;

    InternedString name;
    std::optional<int64_t> explicitValue;

    explicit EnumVariantAST(InternedString n)
        : BaseAST(ASTKind::EnumVariant), name(n) {}

};
using EnumVariantPtr = ASTPtr<EnumVariantAST>;

// ─────────────────────────────────────────────────────────────────────────────
// EnumDeclAST – a named, closed set of integer‑backed constants.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents an enum definition.
 *
 * @example
 *   enum Direction { North, South, East, West }
 *   enum ShaderStage { Vertex = 0x01, Fragment = 0x02, Compute = 0x04 }
 *
 * The semantic pass chooses the backing integer type:
 *   - `byte`  (int8)  for ≤ 255 variants
 *   - `short` (int16) for more
 *
 * Variants are accessed via `EnumName.Variant` dot syntax – never bare names.
 */
struct EnumDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    InternedString name;
    ArenaSpan<EnumVariantPtr> variants;
    Visibility visibility = Visibility::Private;

    EnumDeclAST() : DeclAST(ASTKind::EnumDecl) {}
};
using EnumDeclPtr = ASTPtr<EnumDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TraitMethodAST – a method signature inside a trait (no body).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a single method signature in a trait declaration.
 *
 * @example
 *   draw ()
 *   bounds () -> Rect
 *   compareTo (other T) -> int
 *   clamp (min int)(max int)(value int) -> int    -- curried
 *
 * This node is **not** a MethodDeclAST because it has no body.
 * The semantic pass uses it to verify that impl provides every required method.
 */
struct TraitMethodAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitMethod;

    InternedString name;
    FuncTypeAST funcType;           // full function type (includes qualifiers)

    bool isAsync() const { return funcType.isAsync(); }

    TraitMethodAST() : BaseAST(ASTKind::TraitMethod) {}
};
using TraitMethodPtr = ASTPtr<TraitMethodAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TraitDeclAST – a named method contract (signatures only).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a trait – a set of method signatures that types can implement.
 *
 * @example
 *   trait Drawable { draw (), bounds () -> Rect }
 *   trait Comparable<T> { compareTo (other T) -> int }
 *
 * Used by the semantic pass to:
 *   - Verify that `impl StructName : TraitName` provides every method
 *   - Serve as constraints in generic parameter declarations (`T : Drawable`)
 *
 * Traits are **top‑level only** – cannot be declared inside blocks.
 */
struct TraitDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;   // empty if non‑generic
    ArenaSpan<TraitMethodPtr> methods;
    Visibility visibility = Visibility::Private;

    TraitDeclAST() : DeclAST(ASTKind::TraitDecl) {}
};
using TraitDeclPtr = ASTPtr<TraitDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TraitRefAST – a reference to a trait (with optional generic arguments).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a trait reference in an impl conformance declaration.
 *
 * Grammar: ':' type_path
 *
 * @example
 *   : Drawable          → name="Drawable",    genericArgs={}
 *   : Comparable<int>   → name="Comparable",  genericArgs=[Int]
 *
 * The semantic pass resolves name to a TraitDeclAST and checks that
 * genericArgs count matches the trait's generic parameters.
 */
struct TraitRefAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitRef;

    InternedString name;
    ArenaSpan<TypePtr> genericArgs;   // concrete type arguments

    TraitRefAST() : BaseAST(ASTKind::TraitRef) {}
};
using TraitRefPtr = ASTPtr<TraitRefAST>;

// ─────────────────────────────────────────────────────────────────────────────
// MethodDeclAST – a method body inside an impl block.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a method implementation inside an impl block.
 *
 * Grammar: func_signature '=' func_body
 *
 * @example
 *   length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
 *   offset ~async (dx float)(dy float) -> Point = { ... }
 *
 * Methods are called on values using the colon operator: `value:method(args)`.
 * Inside the method body, the receiver is accessible via the name `self`
 * (or a custom alias if `receiverAlias` was specified in the impl block).
 *
 * No per‑method visibility – visibility is set at the ImplDeclAST level.
 * The body is always a BlockStmtAST (the parser desugars expression bodies).
 */
struct MethodDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MethodDecl;

    InternedString name;
    FuncTypeAST funcType;           // full function type (includes qualifiers)
    StmtPtr body;                   // always BlockStmtAST

    bool isAsync() const { return funcType.isAsync(); }

    MethodDeclAST() : BaseAST(ASTKind::MethodDecl) {}
};
using MethodDeclPtr = ASTPtr<MethodDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromEntryAST – a single conversion entry inside a from block.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents one implicit conversion definition in a `from` block.
 *
 * Grammar: param_group { param_group } '->' type_path '=' func_body
 *
 * @example
 *   (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
 *   (c Celsius)(scale float) -> Fahrenheit = { ... }   -- curried form
 *
 * The parameter groups define the source type(s). The return type must match
 * the target type name of the enclosing FromDeclAST.
 *
 * The semantic pass registers this conversion in the current scope and uses
 * it for implicit casting in:
 *   - variable initialisation
 *   - function arguments
 *   - return statements
 *   - assignments
 */
struct FromEntryAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FromEntry;

    FuncSignature sig;        // parameter groups (source type shape)
    TypePtr returnType;       // must match the target type of FromDeclAST
    StmtPtr body;             // always BlockStmtAST – the conversion logic
    ExprPtr path;                // non‑null for path entry (e.g., IdentifierExprAST)

    FromEntryAST() : BaseAST(ASTKind::FromEntry) {}
};
using FromEntryPtr = ASTPtr<FromEntryAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromDeclAST – defines implicit conversions from one or more source types
//               to a target type.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a `from` block – defines implicit conversions to a type.
 *
 * Grammar: visibility? 'from' type_path [ generic_params ] '{' from_entry* '}'
 *
 * @example
 *   export from Fahrenheit {
 *       (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
 *   }
 *   from Wrapper<T> {
 *       (val T) -> Wrapper<T> = { return Wrapper<T> { value = val } }
 *   }
 *
 * The target type is named by `targetType`. Generic parameters (e.g., <T>)
 * are stored in `genericParams` and are used to instantiate the target type
 * when the conversion is invoked.
 *
 * ## Important Semantic Rules
 *
 * 1. **Target Type**: `targetType` must be a struct type (or a type alias
 *    that resolves to a struct). It cannot be a primitive, trait, or function type.
 *
 * 2. **Generic Consistency**: The number of generic parameters on the
 *    `from` declaration must match the generic arity of the target struct.
 *    Each generic parameter is a fresh binding, not necessarily the same
 *    name used in the struct definition (though convention suggests
 *    consistency).
 *
 * 3. **Overload Resolution**: Multiple `from` declarations for the same
 *    target type are allowed if they accept different source parameter types.
 *    The semantic pass selects the best match based on argument types.
 *
 * 4. **Scoping**: `from` declarations can appear at top‑level (file scope)
 *    or inside any block (local scope). Local `from` declarations are only
 *    visible within that block.
 *
 * 5. **Visibility**: `pub`/`export` modifiers are only allowed at top‑level.
 *    The parser rejects visibility modifiers inside blocks.
 *
 * 6. **Return Type Matching**: Each `FromEntryAST::returnType` must exactly
 *    match the `targetType` of the enclosing `FromDeclAST` after substitution
 *    of generic parameters.
 *
 * 7. **One‑way Conversion**: Conversions are **from** the source type **to**
 *    the target type. There is no automatic reverse conversion – that must
 *    be defined separately with another `from` block.
 */
struct FromDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FromDecl;

    Visibility visibility = Visibility::Private;
    TypePtr targetType;                           // the type being converted to
    ArenaSpan<GenericParamPtr> genericParams;     // e.g., <T> in from Wrapper<T>
    ArenaSpan<FromEntryPtr> entries;              // conversion definitions

    FromDeclAST() : DeclAST(ASTKind::FromDecl) {}
};
using FromDeclPtr = ASTPtr<FromDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// ImplDeclAST – binds method implementations to a type.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents an **impl declaration** – method implementations for a type.
 *
 * Grammar: visibility? 'impl' impl_target
 *          [ impl_generic_params ]
 *          [ 'as' IDENTIFIER ]
 *          [ ':' trait_ref ]
 *          '{' method_decl* '}'
 *
 * impl_target     := type_name | primitive_type
 * impl_generic_params ::= '<' impl_generic_param { ',' impl_generic_param } '>'
 * trait_ref       := IDENTIFIER [ generic_args ]
 *
 * @example
 *   // No alias, no generics, no trait
 *   impl Vec2 {
 *       length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
 *   }
 *   // Usage: let v = Vec2 { x = 3, y = 4 }; let len = v:length()
 *
 *   // With generics and implicit `self` alias
 *   impl Box<T> {
 *       get () -> T = { return self.value }
 *   }
 *
 *   // With generics and explicit alias `b`
 *   impl Box<T> as b {
 *       get () -> T = { return b.value }
 *   }
 *
 *   // Trait conformance with alias
 *   impl Circle as c : Drawable {
 *       draw () { c:render() }
 *   }
 *
 *   // Primitive type impl with alias
 *   impl int as i {
 *       isEven () -> bool = { return i % 2 == 0 }
 *   }
 *   // Usage: 42:isEven()
 *
 *   // Array or function types – must use a type alias first
 *   type IntArray = []int
 *   impl IntArray {
 *       sum () -> int = { ... }
 *   }
 *
 * ## Important Semantic Rules
 *
 * 1. **Call Syntax – Value‑oriented**: Methods are called on a **value**
 *    using the colon operator: `value:method(args)`. The receiver is the value
 *    itself, not the type name. The type of the receiver determines which
 *    `impl` block is consulted.
 *
 * 2. **Target Type**: `targetType` can be any type:
 *    - Struct types (most common)
 *    - Primitive types (`int`, `string`, `bool`, `float`, etc.)
 *    - Enum types
 *    - Type aliases that resolve to a primitive/struct/enum
 *
 *    **Not allowed directly**: array types (`[]T`, `[*]T`, `[N]T`) or
 *    function types. To add methods to an array or a function, first create
 *    a type alias, then impl that alias.
 *
 * 3. **Generic Parameters**: An impl declaration may declare generic parameters
 *    **only when the target type is generic** (generic struct or generic type alias).
 *    The number of generic parameters **must exactly match the arity of the target type**.
 *    The parameter names are fresh bindings (positional correspondence).
 *
 *    @code
 *    struct Box<T> { value T }
 *    impl Box<T> { ... }           // OK: arity 1 matches Box<T>
 *    impl Box<K> { ... }           // OK: different name, same arity
 *    impl Box { ... }              // Error: missing generic parameter
 *    impl Box<T, U> { ... }        // Error: too many parameters
 *    @endcode
 *
 * 4. **Receiver Alias (`as IDENTIFIER`)**: Optional. If omitted, the receiver
 *    is named `self` inside the method bodies. If provided, the given identifier
 *    replaces `self` as the name for the receiver value. The alias is in scope
 *    for all method declarations inside the impl block. It must appear **after**
 *    any generic parameters and **before** an optional trait conformance.
 *
 * 5. **Trait Conformance**: When `traitRef` is present, the impl block
 *    promises to implement every method declared in that trait. The semantic
 *    pass verifies:
 *    - All trait methods are present in `methods`
 *    - Each method signature matches exactly (parameters, return types,
 *      qualifiers like `~async`, `~nullable`, `~parallel`)
 *    - Extra methods are allowed (they are not part of the trait contract)
 *
 * 6. **Method Resolution**: When a method call `value:method()` is encountered,
 *    the semantic pass:
 *    - Determines the static type of `value`
 *    - Looks for an `impl` block whose `targetType` matches that type
 *      (after substituting any generic parameters)
 *    - Finds the method with the given name inside that impl block
 *    - If no matching impl or method is found, emits an error
 *
 * 7. **Self Parameter**: Within method bodies, the receiver value is available
 *    via `self` (or `receiverAlias` if specified). The type of `self` is the
 *    resolved target type (including any generic arguments). The field
 *    `resolvedSelfType` caches this for the semantic pass.
 *
 * 8. **Visibility**: The visibility of the impl block applies to all methods
 *    within it. Individual methods cannot have separate visibility.
 *    `pub`/`export` on an impl block means all its methods are visible
 *    according to that level.
 *
 * 9. **Merging**: Multiple impl blocks for the same target type are allowed
 *    and are merged at semantic time. They can be in different files within
 *    the same package. Method names must be unique across all impl blocks
 *    for a given type (no overloading).
 *
 * 10. **Primitive Methods**: Built‑in primitive types (`int`, `string`, etc.)
 *     can have impl blocks defined by the user. The compiler also provides
 *     a standard set of primitive methods. User‑defined methods cannot
 *     override built‑ins; duplicates cause a semantic error.
 *
 * 11. **Array / Function Types**: Because arrays and function types are
 *     structural (not nominal), they cannot be the target of an impl block
 *     directly. Instead, the programmer must create a type alias:
 *     @code
 *       type IntArray = []int
 *       impl IntArray { ... }
 *     @endcode
 *
 * ## Semantic Caches (filled by TypeResolver)
 *
 * - `resolvedSelfType`: The resolved type of `self` inside method bodies
 * - `resolvedTargetGenericParams`: Generic parameters of the target type
 * - `resolvedSubstitutionMap`: Maps generic param names to concrete types
 *   when the impl is instantiated
 */
struct ImplDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImplDecl;

    InternedString receiverAlias;                     // empty = default "self"
    Visibility visibility = Visibility::Private;
    ArenaSpan<GenericParamPtr> genericParams;         // impl‑level type params
    TypePtr targetType;                               // the type being implemented
    TraitRefPtr traitRef;                             // nullptr if no trait conformance
    ArenaSpan<MethodDeclPtr> methods;                 // method bodies

    // Semantic caches (filled by TypeResolver, Phase 2a)
    mutable TypeAST* resolvedSelfType = nullptr;      // type of 'self' receiver
    mutable const ArenaSpan<GenericParamPtr>* resolvedTargetGenericParams = nullptr;
    mutable std::unordered_map<InternedString, TypeAST*> resolvedSubstitutionMap;

    ImplDeclAST() : DeclAST(ASTKind::ImplDecl) {}
};
using ImplDeclPtr = ASTPtr<ImplDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TypeAliasDeclAST – a named alias for an existing type.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a type alias – does **not** create a new nominal type.
 *
 * @example
 *   type ID          = int
 *   type Callback    = (event Event) -> bool
 *   type Transform<T> = (value T) -> T
 *   type Vec2        = struct { x float, y float }   -- local struct alias
 *
 * `ID` and `int` are interchangeable at the semantic level.
 * For a distinct nominal type, use `struct` instead.
 *
 * Generic parameters allow the alias to be generic:
 *   type Option<T> = struct { value T? }
 *
 * Local type aliases are scoped to the block where they are defined.
 */
struct TypeAliasDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeAliasDecl;

    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;   // empty if non‑generic
    TypePtr aliasedType;                        // the right‑hand side type
    Visibility visibility = Visibility::Private;

    TypeAliasDeclAST() : DeclAST(ASTKind::TypeAliasDecl) {}
};
using TypeAliasDeclPtr = ASTPtr<TypeAliasDeclAST>;