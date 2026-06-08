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
 *     - Methods (MethodDeclAST)
 *     - Enum variants (EnumVariantAST)
 *
 *   TYPE NAMESPACE (TypeDeclAST):
 *     - Structs (StructDeclAST)
 *     - Enums (EnumDeclAST)
 *     - Traits (TraitDeclAST)
 *     - Type aliases (TypeAliasDeclAST)
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

enum class DeclKeyword {
    Let,
    Const
};

// ============================================================================
// DECLARATIONS (alphabetical order)
// ============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// PackageDeclAST
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
 *
 * @note NOT a ValueDeclAST or TypeDeclAST – package declarations are not
 *       looked up by name in the symbol table.
 */
struct PackageDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::PackageDecl;

    InternedString name;

    explicit PackageDeclAST(InternedString n)
        : DeclAST(ASTKind::PackageDecl), name(n) {}
};
using PackageDeclPtr = PackageDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// UseDeclAST
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
 * resolving against the package root.
 *
 * @note NOT a ValueDeclAST or TypeDeclAST – imports are handled by the
 *       module loader, not by normal scope lookup.
 */
struct UseDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UseDecl;

    ArenaSpan<InternedString> path;
    std::optional<InternedString> alias;
    Visibility visibility = Visibility::Private;

    UseDeclAST() : DeclAST(ASTKind::UseDecl) {}
};
using UseDeclPtr = UseDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// VALUE NAMESPACE DECLARATIONS
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
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the resolved type.
 * This is set during Phase 2 type resolution.
 */
struct VarDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    DeclKeyword keyword;
    InternedString name;
    TypeAST* type;          // Original type annotation (may contain aliases)
    ExprAST* init;
    Visibility visibility = Visibility::Private;

    VarDeclAST() : ValueDeclAST(ASTKind::VarDecl) {}
};
using VarDeclPtr = VarDeclAST*;

/**
 * @brief Represents a function parameter.
 *
 * @example
 *   In `add(a int, b int)`, `a` and `b` are ParamAST nodes.
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the parameter's resolved type.
 */
struct ParamAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    InternedString name;
    TypePtr type;           // Original type annotation (may contain aliases)
    bool isVariadic = false;

    ParamAST() : ValueDeclAST(ASTKind::Param) {}
};
using ParamPtr = ParamAST*;
using ParamGroup = std::vector<ParamPtr>;

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
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the field's resolved type.
 */
struct FieldDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    InternedString name;
    TypeAST* type;          // Original type annotation (may contain aliases)
    ExprAST* defaultVal;    // nullptr if no default

    FieldDeclAST() : ValueDeclAST(ASTKind::FieldDecl) {}
};
using FieldDeclPtr = FieldDeclAST*;

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
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to funcType (the function's type).
 * `resolvedReturnType` caches the first return type for quick access during
 * expression checking and overload resolution.
 *
 * @note Visibility is only meaningful at top‑level; inside blocks, the parser
 *       forces Private. Attributes (e.g., @extern, @inline) are stored in
 *       DeclAST::attributes.
 */
struct FuncDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    DeclKeyword keyword;
    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;
    FuncTypeAST* funcType = nullptr;   // full function type (includes qualifiers)
    StmtAST* body = nullptr;           // always BlockStmtAST
    Visibility visibility = Visibility::Private;
    
    // Cache for the first return type (for quick access during checking)
    // Set during Phase 2 type resolution
    TypeAST* resolvedReturnType = nullptr;

    FuncDeclAST() : ValueDeclAST(ASTKind::FuncDecl) {}
};
using FuncDeclPtr = FuncDeclAST*;

/**
 * @brief Represents a method implementation inside an `impl` block.
 *
 * This node supports three syntactic forms defined in the grammar:
 *
 * 1. **Inline Body** – Full signature and a block/expression body.
 *    - `funcType` contains the signature (including qualifiers, parameters, return types).
 *    - `body` is a `BlockStmtAST` (the parser desugars expression bodies).
 *
 * 2. **Plain Assignment** – Method name bound to an existing function.
 *    - `assignmentRef` points to a function reference (`IdentifierExprAST`,
 *      `FieldAccessExprAST`, or a `CallExprAST` for generic instantiation).
 *    - `isInjection = false`, `receiverArg` is empty.
 *
 * 3. **Injection Assignment** – Method name bound to a function where the first
 *    parameter is fixed to the receiver (`self` or an alias).
 *    - `assignmentRef` as in plain assignment.
 *    - `isInjection = true`, `receiverArg` holds the receiver name (must be `self`
 *      or the alias declared on the enclosing `impl` block).
 *
 * Methods are called using the colon operator: `value:method(args)`. The semantic
 * pass resolves the call using the `impl` block that matches the receiver's type.
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the method's type (funcType or
 * the type of assignmentRef).
 *
 * @see ImplDeclAST for the enclosing `impl` block.
 */
struct MethodDeclAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::MethodDecl;

    InternedString name;
    
    // Inline body form
    FuncTypeAST* funcType = nullptr;   // signature + qualifiers
    StmtAST* body = nullptr;

    // Assignment forms (plain or injection)
    ExprAST* assignmentRef = nullptr;
    InternedString receiverArg;
    bool isInjection = false;

    bool isInlineBody() const { return funcType != nullptr; }
    bool isPlainAssignment() const { return assignmentRef != nullptr && !isInjection; }
    bool isInjectionAssignment() const { return isInjection && assignmentRef != nullptr; }

    MethodDeclAST() : ValueDeclAST(ASTKind::MethodDecl) {}
};
using MethodDeclPtr = MethodDeclAST*;

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
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `valueType` (from ValueDeclAST) points to the enum type (the variant's type
 * is the enum itself, not the underlying integer).
 *
 * @note Enum variants are accessed as `EnumName.VariantName` in source.
 *       They live in the value namespace of the enum's scope.
 */
struct EnumVariantAST : ValueDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumVariant;

    InternedString name;
    std::optional<int64_t> explicitValue;

    explicit EnumVariantAST(InternedString n)
        : ValueDeclAST(ASTKind::EnumVariant), name(n) {}
};
using EnumVariantPtr = EnumVariantAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TYPE NAMESPACE DECLARATIONS
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
 * ─── Self‑Type Cache ───────────────────────────────────────────────────────
 * `selfType` (from TypeDeclAST) stores a NamedTypeAST representing the struct
 * itself (e.g., "Vec2" as a type). This is used when the struct name appears
 * in a type annotation or as a constructor.
 *
 * @note The struct's fields are ValueDeclAST nodes stored in the struct's own
 *       scope (not in the global scope). Field lookup occurs via dot notation.
 */
struct StructDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;
    ArenaSpan<FieldDeclPtr> fields;
    Visibility visibility = Visibility::Private;

    StructDeclAST() : TypeDeclAST(ASTKind::StructDecl) {}
};
using StructDeclPtr = StructDeclAST*;

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
 *
 * ─── Self‑Type Cache ───────────────────────────────────────────────────────
 * `selfType` (from TypeDeclAST) stores a NamedTypeAST representing the enum
 * itself (e.g., "Color" as a type).
 */
struct EnumDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    InternedString name;
    ArenaSpan<EnumVariantPtr> variants;
    Visibility visibility = Visibility::Private;
    bool isConst = false;

    EnumDeclAST() : TypeDeclAST(ASTKind::EnumDecl) {}
};
using EnumDeclPtr = EnumDeclAST*;

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
 *
 * ─── Self‑Type Cache ───────────────────────────────────────────────────────
 * `selfType` (from TypeDeclAST) stores a NamedTypeAST representing the trait
 * itself (e.g., "Drawable" as a type).
 */
struct TraitDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;
    ArenaSpan<TraitMethodAST*> methods;
    Visibility visibility = Visibility::Private;

    TraitDeclAST() : TypeDeclAST(ASTKind::TraitDecl) {}
};
using TraitDeclPtr = TraitDeclAST*;

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
 *
 * ─── Semantic Cache ────────────────────────────────────────────────────────
 * `selfType` (from TypeDeclAST) stores a NamedTypeAST representing the alias
 * itself (e.g., "ID" as a type).
 * `resolvedType` (from TypeDeclAST) stores the underlying type after unwrapping
 * all alias chains (e.g., `type A = B; type B = int` → `resolvedType = int`).
 *
 * This eager resolution provides O(1) access to the ultimate type without
 * repeated lookups during type checking.
 */
struct TypeAliasDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeAliasDecl;

    InternedString name;
    ArenaSpan<GenericParamPtr> genericParams;
    TypeAST* aliasedType;       // Original alias target (may be another alias)
    Visibility visibility = Visibility::Private;

    TypeAliasDeclAST() : TypeDeclAST(ASTKind::TypeAliasDecl) {}
};
using TypeAliasDeclPtr = TypeAliasDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TRAIT METHOD (method signature inside trait – not a ValueDeclAST)
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
 *
 * @note Not a ValueDeclAST because trait methods are not directly callable;
 *       they only exist as requirements on implementing types.
 */
struct TraitMethodAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitMethod;

    InternedString name;
    FuncTypeAST* funcType = nullptr;

    TraitMethodAST() : BaseAST(ASTKind::TraitMethod) {}
};
using TraitMethodPtr = TraitMethodAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TRAIT REFERENCE (used in impl : TraitName)
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
    ArenaSpan<TypePtr> genericArgs;

    TraitRefAST() : BaseAST(ASTKind::TraitRef) {}
};
using TraitRefPtr = TraitRefAST*;

// ─────────────────────────────────────────────────────────────────────────────
// FROM BLOCK NODES (conversion definitions)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A single conversion entry inside a from block.
 *
 * Two forms:
 *   - Inline entry: has funcType and body
 *   - Path entry: has path (reference to existing function)
 */
struct FromEntryAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FromEntry;

    FromEntryKind kind = FromEntryKind::Inline;
    
    // For inline entries
    FuncTypeAST* funcType = nullptr;   // the conversion function type
    StmtAST* body = nullptr;
    
    // For path entries
    ExprAST* path = nullptr;

    FromEntryAST() : BaseAST(ASTKind::FromEntry) {}
};
using FromEntryPtr = FromEntryAST*;

/**
 * @brief Defines implicit conversions to a target type.
 *
 * @example
 *   from int {
 *       (s string) -> int = { return #parseInt(s) }
 *   }
 *
 * ─── Target Types ──────────────────────────────────────────────────────────
 * The target type can be:
 *   - Concrete type: `int`, `Vec2`
 *   - Generic array: `[_, <T>]`
 *   - Generic named: `Box<T>` (with generic parameters on the from block)
 */
struct FromDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FromDecl;

    Visibility visibility = Visibility::Private;
    
    // Target information (unified with impl)
    TargetKind targetKind = TargetKind::Concrete;
    TypeAST* targetType = nullptr;
    
    // For GenericArray target
    InternedString arrayTypeParamName;
    
    // For GenericNamed target
    ArenaSpan<GenericParamPtr> genericParams;
    
    ArenaSpan<FromEntryPtr> entries;

    FromDeclAST() : DeclAST(ASTKind::FromDecl) {}
};
using FromDeclPtr = FromDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// IMPL BLOCK NODES (method implementations)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Binds method implementations to a type.
 *
 * @example
 *   impl Vec2 {
 *       length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
 *   }
 *
 *   impl Box<T> as b {
 *       get () -> T = { return b.value }
 *   }
 *
 *   impl [*, <T>] as a {
 *       first () -> T = { return a[0] }
 *   }
 *
 * ─── Target Types ──────────────────────────────────────────────────────────
 * The target type can be:
 *   - Concrete type: `int`, `Vec2`
 *   - Generic array: `[*, <T>]`
 *   - Generic named: `Box<T>` (with generic parameters on the impl block)
 *
 * ─── Trait Conformance ─────────────────────────────────────────────────────
 * If `traitRef` is non-null, this impl implements that trait for the target type.
 * The semantic pass verifies that all trait methods are implemented.
 */
struct ImplDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImplDecl;

    InternedString receiverAlias;           // 'as' alias (replaces 'self')
    Visibility visibility = Visibility::Private;
    
    // Target information (unified with from)
    TargetKind targetKind = TargetKind::Concrete;
    TypeAST* targetType = nullptr;
    
    // For GenericArray target
    InternedString arrayTypeParamName;
    
    // For GenericNamed target
    ArenaSpan<GenericParamPtr> genericParams;
    
    TraitRefPtr traitRef = nullptr;         // Optional trait conformance
    ArenaSpan<MethodDeclPtr> methods;

    ImplDeclAST() : DeclAST(ASTKind::ImplDecl) {}
};
using ImplDeclPtr = ImplDeclAST*;
