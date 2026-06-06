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
 *       not in every BaseAST node.
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
 * resolving against the package root. The alias, if present, becomes the
 * name by which the imported module is referenced.
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
// VarDeclAST
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

    DeclKeyword keyword;
    InternedString name;
    TypeAST* type;
    ExprAST* init;
    Visibility visibility = Visibility::Private;
    bool isConst = false;

    VarDeclAST() : DeclAST(ASTKind::VarDecl) {}
};
using VarDeclPtr = VarDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// FuncDeclAST
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
    ArenaSpan<GenericParamPtr> genericParams;
    FuncTypeAST* funcType;
    StmtAST* body;
    Visibility visibility = Visibility::Private;
    bool isConst = false;

    bool isAsync()   const { return funcType->isAsync(); }
    bool hasParams() const { return funcType->sig.hasParams(); }

    FuncDeclAST() : DeclAST(ASTKind::FuncDecl) {}
};
using FuncDeclPtr = FuncDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// FieldDeclAST
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
    TypeAST* type;
    ExprAST* defaultVal;

    FieldDeclAST() : BaseAST(ASTKind::FieldDecl) {}
};
using FieldDeclPtr = FieldDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// StructDeclAST
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
    ArenaSpan<GenericParamPtr> genericParams;
    ArenaSpan<FieldDeclPtr> fields;
    Visibility visibility = Visibility::Private;
    mutable NamedTypeAST* selfType = nullptr;

    StructDeclAST() : DeclAST(ASTKind::StructDecl) {}
};
using StructDeclPtr = StructDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// EnumVariantAST
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
    bool isConst = false;

    explicit EnumVariantAST(InternedString n)
        : BaseAST(ASTKind::EnumVariant), name(n) {}
};
using EnumVariantPtr = EnumVariantAST*;

// ─────────────────────────────────────────────────────────────────────────────
// EnumDeclAST
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
    bool isConst = false;

    EnumDeclAST() : DeclAST(ASTKind::EnumDecl) {}
};
using EnumDeclPtr = EnumDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TraitMethodAST
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
    FuncTypeAST* funcType;

    bool isAsync() const { return funcType->isAsync(); }

    TraitMethodAST() : BaseAST(ASTKind::TraitMethod) {}
};
using TraitMethodPtr = TraitMethodAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TraitDeclAST
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
    ArenaSpan<GenericParamPtr> genericParams;
    ArenaSpan<TraitMethodPtr> methods;
    Visibility visibility = Visibility::Private;

    TraitDeclAST() : DeclAST(ASTKind::TraitDecl) {}
};
using TraitDeclPtr = TraitDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TraitRefAST
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
// MethodDeclAST – method inside impl block
// ─────────────────────────────────────────────────────────────────────────────

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
 * @field name            Method name.
 * @field funcType        Non‑null for inline body form; holds signature and qualifiers.
 * @field body            Non‑null for inline body form; block statement of the method.
 * @field assignmentRef   Non‑null for assignment forms; expression that yields a function.
 * @field receiverArg     For injection assignment, the receiver name (empty otherwise).
 * @field isInjection     True for injection assignment form.
 *
 * @see ImplDeclAST for the enclosing `impl` block.
 */
struct MethodDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MethodDecl;

    InternedString name;
    
    // Inline body form
    FuncTypeAST* funcType = nullptr;
    StmtAST* body = nullptr;

    // Assignment forms (plain or injection)
    ExprAST* assignmentRef = nullptr;
    InternedString receiverArg;
    bool isInjection = false;

    bool isInlineBody() const { return funcType != nullptr; }
    bool isPlainAssignment() const { return assignmentRef != nullptr && !isInjection; }
    bool isInjectionAssignment() const { return isInjection && assignmentRef != nullptr; }
    bool isAsync() const { return funcType && funcType->isAsync(); }

    MethodDeclAST() : BaseAST(ASTKind::MethodDecl) {}
};
using MethodDeclPtr = MethodDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// FromEntryAST – a single conversion entry inside a from block
// ─────────────────────────────────────────────────────────────────────────────

struct FromEntryAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FromEntry;

    FromEntryKind kind = FromEntryKind::Inline;
    
    // For inline entries
    FuncSignature sig;
    TypeAST* returnType = nullptr;
    StmtAST* body = nullptr;
    
    // For path entries
    ExprAST* path = nullptr;
    
    // Qualifiers inferred from path entry (set by semantic pass)
    bool isAsync = false;
    bool isNullable = false;

    FromEntryAST() : BaseAST(ASTKind::FromEntry) {}
};
using FromEntryPtr = FromEntryAST*;

// ─────────────────────────────────────────────────────────────────────────────
// FromDeclAST – defines implicit conversions to a type
// ─────────────────────────────────────────────────────────────────────────────

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
// ImplDeclAST – binds method implementations to a type
// ─────────────────────────────────────────────────────────────────────────────

struct ImplDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImplDecl;

    InternedString receiverAlias;
    Visibility visibility = Visibility::Private;
    
    // Target information (unified with from)
    TargetKind targetKind = TargetKind::Concrete;
    TypeAST* targetType = nullptr;
    
    // For GenericArray target
    InternedString arrayTypeParamName;
    
    // For GenericNamed target
    ArenaSpan<GenericParamPtr> genericParams;
    
    TraitRefPtr traitRef = nullptr;
    ArenaSpan<MethodDeclPtr> methods;

    // Semantic caches
    mutable TypeAST* resolvedSelfType = nullptr;
    mutable const ArenaSpan<GenericParamPtr>* resolvedTargetGenericParams = nullptr;
    mutable std::unordered_map<InternedString, TypeAST*> resolvedSubstitutionMap;

    ImplDeclAST() : DeclAST(ASTKind::ImplDecl) {}
};
using ImplDeclPtr = ImplDeclAST*;

// ─────────────────────────────────────────────────────────────────────────────
// TypeAliasDeclAST
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
    ArenaSpan<GenericParamPtr> genericParams;
    TypeAST* aliasedType;
    Visibility visibility = Visibility::Private;

    TypeAliasDeclAST() : DeclAST(ASTKind::TypeAliasDecl) {}
};
using TypeAliasDeclPtr = TypeAliasDeclAST*;