/**
 * @file DeclAST.hpp
 *
 * @responsibility Defines nodes that create new entities (Functions, Structs, Variables).
 *
 * @hierarchy BaseAST -> DeclAST -> [Concrete Nodes]
 *
 * @related_files
 *
 *   - src/parser/ParserDecl.cpp (The primary producer)
 *   - src/semantic/ (The primary consumer for symbol table population)
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Visibility — the three visibility tiers for declarations.
//   Private — visible only within the file (default)
//   Package — visible to the entire package (Visibility)
//   Export  — visible to external consumers (export)
// ─────────────────────────────────────────────────────────────────────────────

enum class Visibility { 
    Private,
    Package,
    Export 
};

// ─────────────────────────────────────────────────────────────────────────────
// DeclKeyword — the three variable / function declaration keywords.
// Stored on VarDeclAST and FuncDeclAST so the semantic pass can enforce
// reassignability and nil rules without touching TokenType.
// ─────────────────────────────────────────────────────────────────────────────

enum class DeclKeyword {
    Let,   // reassignable, mutable in place, nil allowed
    Const, // not reassignable, not mutable in place, nil allowed
};

// ─────────────────────────────────────────────────────────────────────────────
// PackageDeclAST
//
// The first non-comment line of every .luc file.
//   package math
//
// Exactly one per file — the parser enforces this. ProgramAST stores the
// package name as a plain string; this node carries the source location for
// error reporting when the name mismatches the directory.
// ─────────────────────────────────────────────────────────────────────────────
struct PackageDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::PackageDecl;

    InternedString name; // "math", "renderer", "app", ...

    explicit PackageDeclAST(InternedString n) : DeclAST(ASTKind::PackageDecl), name(n) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// UseDeclAST
//
// Imports a module path into the current file, with an optional local alias.
//   use math.vec2           →  path = ["math","vec2"],  alias = nullopt
//   use math as m           →  path = ["math"],         alias = "m"
//   use renderer.types      →  path = ["renderer","types"]
//
// path stores the segments split on '.'. The semantic pass joins them back
// when resolving against the package root.
// ─────────────────────────────────────────────────────────────────────────────
struct UseDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UseDecl;

    std::vector<InternedString> path;    // e.g. ["math", "vec2"]
    std::optional<InternedString> alias; // present when `as IDENTIFIER` was written
    Visibility visibility = Visibility::Private;

    UseDeclAST() : DeclAST(ASTKind::UseDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// VarDeclAST
//
// A variable declaration — let or const.
//   let   count int     = 0
//   const PI    float   = 3.14159
//   const MAX   int     = 65536
//   let   name  string? = nil
//
// Type annotation is always required in Luc — type is never null here.
// init is null when no initialiser was written (valid for let, invalid for
// const — the semantic pass enforces that const always has an initialiser).
// visibility tracks the visibility modifier when used at top level.
// ─────────────────────────────────────────────────────────────────────────────
struct VarDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    DeclKeyword keyword; // Let / Const
    InternedString name;
    TypePtr type;        // always present — annotation is required
    ExprPtr init;        // nullptr if no initialiser was written
    Visibility visibility = Visibility::Private;

    VarDeclAST() : DeclAST(ASTKind::VarDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamAST
//
// A single generic type parameter on a declaration.
//   T                              →  unconstrained
//   T : Drawable                   →  single constraint
//   T : Drawable + Hashable        →  multiple constraints (stored as a list)
//   K : Hashable + Comparable, V   →  two separate GenericParamASTs
//
// constraints stores the trait names as plain strings — the semantic pass
// resolves each name to a TraitDeclAST in the symbol table.
// ─────────────────────────────────────────────────────────────────────────────
struct GenericParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParam;

    InternedString name;                     // "T", "K", "V"
    std::vector<InternedString> constraints; // trait names — empty if unconstrained

    explicit GenericParamAST(InternedString n) : BaseAST(ASTKind::GenericParam), name(n) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

struct ParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    InternedString name;
    TypePtr     type;
    bool        isVariadic = false;

    ParamAST() : BaseAST(ASTKind::Param) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using ParamPtr   = ASTPtr<ParamAST>;
using ParamGroup = std::vector<ParamPtr>;

using GenericParamPtr = ASTPtr<GenericParamAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FuncDeclAST
//
// A function declaration — may appear at top‑level or inside any block.
//   let add (a int) (b int) int = { return a + b }
//   let fetch ~async (url string) -> string = { return await httpGet(url) }
//   @extern("printf") const printf (fmt *uint8, args ...any) -> int   // no body
//
// The function's complete signature (parameters, return types, qualifiers)
// is stored in the `sig` field (FuncSignature). The body is always a
// BlockStmtAST (the parser desugars expression bodies into `return expr`).
//
// Visibility (pub/export) is only meaningful at top‑level; inside blocks,
// the parser forces Visibility::Private.
//
// Attributes (e.g., @extern, @inline) are stored in BaseAST::attributes.
//
// For local function declarations (inside a block), the function is scoped
// to that block and cannot be accessed outside.
// ─────────────────────────────────────────────────────────────────────────────
struct FuncDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    DeclKeyword keyword;
    InternedString name;
    std::vector<GenericParamPtr> genericParams;
    FuncSignature sig;
    StmtPtr body;                                   // always BlockStmtAST
    Visibility visibility = Visibility::Private;

    // Convenience helpers
    bool isAsync() const { return sig.isAsync(); }
    bool hasParams() const { return sig.hasParams(); }
    
    FuncDeclAST() : DeclAST(ASTKind::FuncDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using FuncDeclPtr = ASTPtr<FuncDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FieldDeclAST
//
// A single field inside a struct body.
//   x     float           →  name="x",  type=Float,  defaultVal=nullptr
//   r     float = 1.0     →  name="r",  type=Float, defaultVal=LiteralExpr(1.0)
//   items [*]string       →  name="items", type=DynamicArray(String)
//
// defaultVal is null when no default was written. The semantic pass enforces
// that struct literals must supply every field without a default.
// ─────────────────────────────────────────────────────────────────────────────
struct FieldDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FieldDecl;

    InternedString name;
    TypePtr type;
    ExprPtr defaultVal; // nullptr if no default

    FieldDeclAST() : BaseAST(ASTKind::FieldDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using FieldDeclPtr = ASTPtr<FieldDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// StructDeclAST
//
// A struct type declaration.
//   Visibility struct Vec2 { x float  y float }
//   struct Scene<T : Drawable> { objects []T }
//   struct Cache<K : Hashable + Comparable, V> { keys []K  values []V }
//
// Impl blocks are separate top-level declarations (ImplDeclAST) — the struct
// itself only holds its fields and generic params.
//
// ─────────────────────────────────────────────────────────────────────────────
// !!! read this, this bug is very hard to fix !!!
// selfType field (Semantic Phase):
//   During Phase 1 (SemanticCollector), each struct declaration needs a type
//   representation in the symbol table so that:
//   (1) Struct literal expressions can return a type: `Point { x = 1 y = 2 }`
//   (2) Variable assignments can match the struct type: `let p Point = ...`
//   (3) Type checkers can compare struct types for compatibility
//
//   The selfType is a NamedTypeAST synthesized on-demand that represents
//   the struct itself as a type (e.g., struct name "Point" becomes a type).
//
// Why 'mutable' and 'unique_ptr':
//   - mutable: Allows lazy initialization from within const contexts
//     (e.g., during visitor traversal where the AST is const).
//     The field is logically part of semantic-phase bookkeeping, not the
//     declaration itself, so mutability is appropriate.
//
//   - unique_ptr: Provides stable ownership of the synthesized NamedTypeAST.
//     The Symbol table stores a raw pointer to selfType.get(), which remains
//     valid for the lifetime of this StructDeclAST. Using unique_ptr ensures
//     the allocation is cleaned up when the struct declaration is destroyed.
//
// Initialization:
//   - Created lazily in SemanticCollector::visit(StructDeclAST&)
//   - Once created, remains constant for the semantic pass
//   - Stored in Symbol::type when declaring the struct symbol
// ─────────────────────────────────────────────────────────────────────────────
struct StructDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    InternedString name;
    std::vector<GenericParamPtr> genericParams; // empty if non-generic
    std::vector<FieldDeclPtr> fields;
    Visibility visibility = Visibility::Private;
    
    // SEMANTIC PHASE (Phase 1+): A synthesized NamedTypeAST representing
    // the struct as a type. Initialized by SemanticCollector, then stored
    // as the symbol's type in the symbol table. Enables:
    //   - struct literal type resolution (checkStructLiteralExpr returns this)
    //   - variable-to-struct type assignments (checkVarDecl type matching)
    //   - struct type identity in the type system
    //
    // Why NOT initialized in constructor:
    //   - Would require TypeAST allocation on every struct, even in early parse phases
    //   - The name is a string, not known until after parsing
    //   - Lazy initialization (first access by SemanticCollector) is efficient
    mutable ASTPtr<NamedTypeAST> selfType;

    StructDeclAST() : DeclAST(ASTKind::StructDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// EnumVariantAST
//
// A single named constant inside an enum.
//   North         →  name="North",  explicitValue=nullopt   (auto-assigned)
//   Vertex = 0x01 →  name="Vertex", explicitValue=1
//
// explicitValue is nullopt when nothing was written after the name.
// The semantic pass computes the final integer value for every variant:
//   - auto starts at 0, increments by 1 from the previous variant
//   - explicit value resets the counter from that point
// Duplicate values within the same enum are a semantic error.
// ─────────────────────────────────────────────────────────────────────────────
struct EnumVariantAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::EnumVariant;

    InternedString name;
    std::optional<int64_t> explicitValue; 

    explicit EnumVariantAST(InternedString n) : BaseAST(ASTKind::EnumVariant), name(n) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using EnumVariantPtr = ASTPtr<EnumVariantAST>;

// ─────────────────────────────────────────────────────────────────────────────
// EnumDeclAST
//
// A named, closed set of integer-backed constants.
//   Visibility enum Direction { North  South  East  West }
//   Visibility enum ShaderStage { Vertex = 0x01  Fragment = 0x02  Compute = 0x04 }
//
// The semantic pass chooses the backing integer type:
//   - byte  (int8)  for ≤ 255 variants
//   - short (int16) for more
// Variants are accessed via EnumName.Variant dot syntax — never bare names.
// ─────────────────────────────────────────────────────────────────────────────
struct EnumDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    InternedString name;
    std::vector<EnumVariantPtr> variants;
    Visibility visibility = Visibility::Private;

    EnumDeclAST() : DeclAST(ASTKind::EnumDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TraitMethodAST
//
// A single method signature inside a trait – no body, no `=`.
//   draw   ()
//   bounds () -> Rect
//   compareTo (other T) -> int
//   clamp (min int)(max int)(value int) -> int    -- curried method signature
//
// paramGroups mirrors FuncSignature: outer vector = curry groups, inner vector = params.
// Single group = normal method; multiple groups = curried method signature.
// This node is **not** a MethodDeclAST because it has no body.
//
// The semantic pass uses this node when checking that an ImplDeclAST provides
// every method required by the trait. ReturnTypes may be empty for void methods.
// Qualifiers (~async, ~nullable, ~parallel) are stored in sig.qualifiers/rawQualifiers.
//
// Traits themselves are top‑level only – they cannot be declared locally.
// ─────────────────────────────────────────────────────────────────────────────
struct TraitMethodAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitMethod;

    InternedString name;
    FuncSignature sig;

    // Convenience helpers
    bool isAsync() const { return sig.isAsync(); }
    
    TraitMethodAST() : BaseAST(ASTKind::TraitMethod) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using TraitMethodPtr = ASTPtr<TraitMethodAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TraitDeclAST
//
// A named method contract – signatures only, no bodies, no fields.
//   Visibility trait Drawable { draw ()  bounds () -> Rect }
//   Visibility trait Comparable<T> { compareTo (other T) -> int }
//
// Used by the semantic pass to:
//   - Verify that `impl StructName : TraitName` provides every method.
//   - Serve as constraints in generic parameter declarations (`T : Drawable`).
//
// Traits are **top‑level only** – they cannot be declared inside blocks.
// This restriction is enforced by the parser (parseDeclaration with
// DeclContext::Local rejects TRAIT tokens).
// ─────────────────────────────────────────────────────────────────────────────
struct TraitDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    InternedString name;
    std::vector<GenericParamPtr> genericParams; // empty if non-generic
    std::vector<TraitMethodPtr> methods;
    Visibility visibility = Visibility::Private;

    TraitDeclAST() : DeclAST(ASTKind::TraitDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TraitRefAST
//
// The trait reference in an impl conformance declaration — the ": Drawable"
// or ": Comparable<int>" part.
//   : Drawable          →  name="Drawable",    genericArgs={}
//   : Comparable<int>   →  name="Comparable",  genericArgs=[Int]
//
// Now a full BaseAST node with visitor support, allowing the semantic pass
// to walk impl conformance declarations uniformly.
// The semantic pass resolves name to a TraitDeclAST and checks that
// genericArgs count matches the trait's generic params.
// ─────────────────────────────────────────────────────────────────────────────
struct TraitRefAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitRef;
    InternedString name;                    // trait name, e.g. "Comparable"
    std::vector<TypePtr> genericArgs;       // concrete args, e.g. [Int] for Comparable<int>

    TraitRefAST() : BaseAST(ASTKind::TraitRef) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using TraitRefPtr = ASTPtr<TraitRefAST>;

// ─────────────────────────────────────────────────────────────────────────────
// MethodDeclAST
//
// A method body inside an impl block (top‑level or local impl).
//   length () -> float = { return #sqrt(x*x + y*y) }
//   offset ~async (dx float)(dy float) -> Point = { ... }
//
// No per‑method visibility prefix – visibility is set at the ImplDeclAST level.
// The method name is separate from the signature; qualifiers belong to the
// method binding (e.g., ~async, ~nullable, ~parallel) and are stored in `sig`.
//
// The body is always a BlockStmtAST (the parser desugars expression bodies).
//
// Methods can appear in impl blocks that are local (inside a function) as well
// as top‑level impl blocks. The semantic pass resolves the method name against
// the struct type and checks signature compatibility.
// ─────────────────────────────────────────────────────────────────────────────
struct MethodDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MethodDecl;

    InternedString name;
    FuncSignature sig;
    StmtPtr body;                                   // always BlockStmtAST

    // Convenience helpers
    bool isAsync() const { return sig.isAsync(); }
    
    MethodDeclAST() : BaseAST(ASTKind::MethodDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using MethodDeclPtr = ASTPtr<MethodDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromEntryAST
//
// A single conversion entry inside a from block (top‑level or local from).
//   (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
//   (c Celsius)(scale float) -> Fahrenheit = { ... }   -- curried
//
// Grammar: param_group { param_group } '->' type '=' func_body
//
// The parameter groups define the source value(s). The return type (stored in
// `returnType`) must match the target struct name of the enclosing FromDeclAST.
// The `->` arrow is mandatory and is consumed by the parser before parsing the
// return type.
//
// From entries can appear in local from blocks (inside a function) as well as
// top‑level. The semantic pass registers the conversion in the current scope
// (file or block) and uses it for implicit casting in variable initialisation,
// function arguments, returns, and assignments.
// ─────────────────────────────────────────────────────────────────────────────
struct FromEntryAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FromEntry;

    FuncSignature   sig;
    TypePtr         returnType;                   
    StmtPtr         body;                               // always BlockStmtAST

    FromEntryAST() : BaseAST(ASTKind::FromEntry) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using FromEntryPtr = ASTPtr<FromEntryAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromDeclAST
//
// A type conversion block – defines implicit conversions from a source type
// to a target struct type. May appear at top‑level or inside any block.
//
//   export from Fahrenheit {
//       (c Celsius) -> Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
//       (k Kelvin)  -> Fahrenheit = { return Fahrenheit { value = (k.value - 273.15) * 9/5 + 32 } }
//   }
//
//   from Wrapper<T> { (val T) -> Wrapper<T> = { return Wrapper<T> { value = val } } }
//
// The target struct type is named by `targetTypeName`. Generic parameters
// (e.g., <T>) are stored in `genericParams` and are used to instantiate the
// target type when the conversion is invoked.
//
// Visibility (`pub`/`export`) is only meaningful at top‑level; when used
// locally, the parser forces Visibility::Private and rejects visibility
// modifiers. Attributes are allowed on from blocks in any context.
//
// Multiple from declarations for the same target struct are allowed if they
// accept different source parameter types.
// ─────────────────────────────────────────────────────────────────────────────
struct FromDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FromDecl;

    Visibility visibility = Visibility::Private;
    TypePtr targetType; // e.g., NamedTypeAST("Wrapper") with genericArgs = [ NamedTypeAST("T") ]
    std::vector<GenericParamPtr> genericParams;  // e.g. <T> in from Unwrapped<T>
    std::vector<FromEntryPtr> entries;

    FromDeclAST() : DeclAST(ASTKind::FromDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ImplDeclAST
//
// Binds methods (and from conversions) to a struct type.
//   Visibility impl Vec2 { ... }
//   Visibility impl Circle : Drawable { ... }
//   Visibility impl Scene<T : Drawable> { ... }
//   impl Vec2 { ... }   -- package-private methods
//
// isVisibility — true = methods callable by anyone holding the value
//         false = methods callable only within the package
//
// genericParams — type params on the impl itself, e.g. <T : Drawable>
//
// structName — the name of the type being implemented ("Vec2", "Scene")
//
// structGenericArgs — the type args supplied to the struct name, e.g. the
//   <T> in  impl Scene<T : Drawable>. These bind the generic param T
//   declared above to the struct's type parameter.
//
// traitRef — present when ": TraitName" was written; nullptr otherwise.
//   The semantic pass uses this to verify full trait conformance.
//
// methods — regular method bodies
//
// Multiple impl blocks for the same struct merge at semantic time.
// ─────────────────────────────────────────────────────────────────────────────
struct ImplDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImplDecl;

    InternedString receiverAlias;   // empty = default "self"
    Visibility visibility = Visibility::Private;
    std::vector<GenericParamPtr> genericParams; // impl-level type params
    TypePtr targetType;
    TraitRefPtr traitRef;                       // nullptr if no conformance
    std::vector<MethodDeclPtr> methods;

    // ── Phase 2a cache (filled by TypeResolver) ──────────────────────────────
    mutable TypeAST* resolvedSelfType = nullptr;                     // the type of 'self'
    mutable const std::vector<GenericParamPtr>* resolvedTargetGenericParams = nullptr;
    mutable std::unordered_map<InternedString, TypeAST*> resolvedSubstitutionMap;

    ImplDeclAST() : DeclAST(ASTKind::ImplDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ExtensionDeclAST
//
// A static method namespace block – adds namespaced functions to an existing type.
//   type Int = int -- type alias on primitive int
//   extension Int std {
//       abs (x int) -> int = { return #abs(x) }
//       clamp (x int, lo int, hi int) -> int = { ... }
//   }
//
// Grammar: [visibility_mod] 'extension' type_path IDENTIFIER '{' { func_decl } '}'
//   - type_path: the type being extended (e.g., Int, Vec2, Wrapper<int>)
//   - IDENTIFIER: the namespace name (e.g., "std", "math", "bitops")
//   - func_decl: each function is a static method – no self parameter
//
// Visibility (pub/export) controls accessibility of all methods in the block.
// Local extension blocks (inside a function) must omit visibility modifiers.
// Duplicate method signatures within the same extension block are forbidden.
// Multiple extension blocks for the same type are allowed with different namespaces.
//
// The compiler resolves Type::namespace.method by looking up the mangled symbol.
// Mangled name format: Type::namespace.method
//
// Call site examples:
//   Int::std.abs(-5)
//   Vec2::math.normalize(point)
//   Wrapper<int>::container.unwrap(wrapped)
// ─────────────────────────────────────────────────────────────────────────────
struct ExtensionDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ExtensionDecl;

    Visibility visibility = Visibility::Private;
    TypePtr targetType;                     // the type being extended (e.g., int, Vec2)
    InternedString namespaceName;           // the namespace identifier (e.g., "std", "math")
    std::vector<FuncDeclPtr> methods;       // static method declarations
    std::vector<GenericParamPtr> genericParams; // generic parameters for the extension block
                                                // (e.g., extension Wrapper<T> container { ... })

    ExtensionDeclAST() : DeclAST(ASTKind::ExtensionDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeAliasDeclAST
//
// A named alias for an existing type shape. May appear at top‑level or inside
// any block (local type alias).
//
//   type ID          = int
//   type Callback    = (event Event) -> bool
//   type Transform<T> = (value T) -> T
//   type Vec2        = struct { x float, y float }   -- local struct alias
//
// Does **not** create a new nominal type – `ID` and `int` are interchangeable
// at the semantic level. Use `struct` for a distinct nominal type.
//
// `aliasedType` holds the full TypeAST on the right‑hand side of the `=`.
// Generic parameters (stored in `genericParams`) allow the alias to be
// generic, e.g., `type Option<T> = struct { value T? }`.
//
// Visibility (`pub`/`export`) is only meaningful at top‑level; when used
// locally, the parser forces Visibility::Private and rejects visibility
// modifiers. Attributes are allowed on type aliases in any context.
//
// Local type aliases are scoped to the block in which they are defined and
// are not visible outside that block. The semantic pass resolves the alias
// by substituting the aliased type during type checking.
// ─────────────────────────────────────────────────────────────────────────────
struct TypeAliasDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeAliasDecl;

    InternedString name;
    std::vector<GenericParamPtr> genericParams; // empty if non-generic
    TypePtr aliasedType;                        // the right-hand side
    Visibility visibility = Visibility::Private;

    TypeAliasDeclAST() : DeclAST(ASTKind::TypeAliasDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};