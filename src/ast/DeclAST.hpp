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
// DeclAST.hpp — all declaration nodes
//
// Every node here inherits from DeclAST (defined in BaseAST.hpp).
// StmtAST.hpp includes this header because statements can contain local
// declarations (var_decl, func_decl inside a block).
//
// Forward note on bodies:
//   FuncDeclAST, MethodDeclAST, and FromDeclAST all hold a body. The body
//   type is StmtPtr (a BlockStmtAST in practice). Because StmtAST.hpp
//   includes DeclAST.hpp, we cannot include StmtAST.hpp here — that would
//   be circular. Instead the body is stored as a forward-declared StmtPtr.
//   The parser fills it in after parsing the block. The semantic pass and
//   codegen both have StmtAST.hpp in scope and read it without issues.
//
// Node inventory:
//   PackageDeclAST      — package foo
//   UseDeclAST          — use math.vec2 [as m]
//   ModuleDeclAST       — module math { use math.vec2 }
//   VarDeclAST          — let / imt / val  name  type  [= expr]
//   ParamAST            — name type  or  name ...type  (parameter of a function)
//   GenericParamAST     — T  or  T : Trait  or  T : A + B 
//   FuncDeclAST         — let/imt/val  name  [<generics>]  (group)+ [returnType]  = body
//   StructDeclAST       — [pub] struct Name [<generics>] { fields }
//   FieldDeclAST        — name type [= defaultExpr]
//   EnumDeclAST         — [pub] enum Name { variants }
//   EnumVariantAST      — VariantName [= INT_LITERAL]
//   TraitDeclAST        — [pub] trait Name [<generics>] { method signatures }
//   TraitMethodAST      — name (params) [returnType]   (signature only, no body)
//   ImplDeclAST         — [pub] impl [<generics>] Name [: TraitRef] { members }
//   TraitRefAST         — TraitName [<genericArgs>]    (the ": Drawable" part)
//   MethodDeclAST       — name (params) [returnType] = body
//   FromDeclAST         — from (paramName paramType) returnType = body
//   TypeAliasDeclAST    — [pub] type Name [<generics>] = TypeAST
//   ExternDeclAST       — extern let name (params) [returnType]
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Visibility — the three visibility tiers for declarations.
//   Private — visible only within the file (default)
//   Package — visible to the entire package (pub)
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
    Let, // reassignable, mutable in place, nil allowed
    Imt, // not reassignable, not mutable in place, nil allowed
    Val, // not reassignable, not mutable in place, nil FORBIDDEN in type tree
};

// ─────────────────────────────────────────────────────────────────────────────
// FuncBodyKind — which syntactic form was used for the function body.
// The parser sets this so the semantic pass knows what it is walking.
//
//   Block    — let f (x int) int = { return x + 1 }
//   AnonFunc — let f (x int) int = (x int) int { return x + 1 }   (verbose form)
//
// Note: match-as-body and if-as-body are sugar — the parser desugars them
// into a BlockStmtAST containing a single MatchExprAST / IfExprAST statement
// before storing, so the AST always holds a block. FuncBodyKind::Block covers
// all three of those forms from the parser's perspective.
// async bodies are indicated by the isAsync flag on FuncDeclAST /
// MethodDeclAST.
// ─────────────────────────────────────────────────────────────────────────────

enum class FuncBodyKind {
    Block,    // standard expr_block body: = { ... }
    AnonFunc, // explicit anonymous function: = (params) ret { ... }
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

    std::string name; // "math", "renderer", "app", ...

    explicit PackageDeclAST(std::string n)
        : DeclAST(ASTKind::PackageDecl), name(std::move(n)) {}

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

    std::vector<std::string> path;    // e.g. ["math", "vec2"]
    std::optional<std::string> alias; // present when `as IDENTIFIER` was written
    Visibility visibility = Visibility::Private;

    UseDeclAST() : DeclAST(ASTKind::UseDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// VarDeclAST
//
// A variable declaration — let, imt, or val.
//   let  count int     = 0
//   imt  PI    float   = 3.14159
//   val  MAX   int     = 65536
//   let  name  string? = nil
//
// Type annotation is always required in Luc — type is never null here.
// init is null when no initialiser was written (valid for let, invalid for
// val — the semantic pass enforces that val always has an initialiser).
// visibility tracks the visibility modifier when used at top level.
// ─────────────────────────────────────────────────────────────────────────────

struct VarDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::VarDecl;

    DeclKeyword keyword; // Let / Imt / Val
    std::string name;
    TypePtr type;        // always present — annotation is required
    ExprPtr init;        // nullptr if no initialiser was written
    Visibility visibility = Visibility::Private;

    VarDeclAST() : DeclAST(ASTKind::VarDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ParamAST
//
// A single parameter in a parameter list.
//   name  type           →  positional:  x int,  other Vec2
//   name  ...type        →  variadic:    args ...int
//
// Variadic params must be last in the list — enforced by the parser.
// ParamAST is not a DeclAST because it never stands alone as a top-level
// declaration — it is always owned by a FuncDeclAST, MethodDeclAST,
// TraitMethodAST, or FuncTypeAST.
// ─────────────────────────────────────────────────────────────────────────────

struct ParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    std::string name;
    TypePtr type;
    bool isVariadic = false; // true → args ...int

    ParamAST() : BaseAST(ASTKind::Param) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using ParamPtr = std::unique_ptr<ParamAST>;

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

    std::string name;                     // "T", "K", "V"
    std::vector<std::string> constraints; // trait names — empty if unconstrained

    explicit GenericParamAST(std::string n)
        : BaseAST(ASTKind::GenericParam), name(std::move(n)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using GenericParamPtr = std::unique_ptr<GenericParamAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FuncDeclAST
//
// A function declaration — syntactic sugar for a variable holding a function.
//   let add (a int) (b int) int    = { return a + b }
//   imt greet (name string)        = { io.printl("hi " + name) }
//   let fetch (url string) string  = async { return await httpGet(url) }
//
// paramGroups — multiple groups = curried function. Each group is a list of
//   ParamAST. The semantic pass desugars multi-group functions into nested
//   anonymous functions. Single-group = normal function.
//
// returnType — nullptr means void (no return value).
//   For multiple return values use a tuple return type: (int, string).
//
// isAsync — true when the body was written as  = async { ... }
//   or  = async (params) ret { ... }
//
// body — always a BlockStmtAST. match/if direct bodies are desugared by the
//   parser into a single-statement block before storing here.
//
// bodyKind — records which syntactic form was written (for LSP / pretty print).
//
// visibility — the visibility modifier on the declaration.
//
// Generics: let process<T : Numeric> (x T) string = { ... }
//   genericParams holds the T declarations.
// ─────────────────────────────────────────────────────────────────────────────

struct FuncDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FuncDecl;

    DeclKeyword keyword;
    std::string name;
    std::vector<GenericParamPtr> genericParams;     // empty if non-generic
    std::vector<std::vector<ParamPtr>> paramGroups; // outer = curry groups
    TypePtr returnType;                             // nullptr = void
	StmtPtr body;                                   // BlockStmtAST
    FuncBodyKind bodyKind = FuncBodyKind::Block;
    bool isAsync = false;
    Visibility visibility = Visibility::Private;

    FuncDeclAST() : DeclAST(ASTKind::FuncDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

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

    std::string name;
    TypePtr type;
    ExprPtr defaultVal; // nullptr if no default

    FieldDeclAST() : BaseAST(ASTKind::FieldDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using FieldDeclPtr = std::unique_ptr<FieldDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// StructDeclAST
//
// A struct type declaration.
//   pub struct Vec2 { x float  y float }
//   struct Scene<T : Drawable> { objects []T }
//   struct Cache<K : Hashable + Comparable, V> { keys []K  values []V }
//
// Impl blocks are separate top-level declarations (ImplDeclAST) — the struct
// itself only holds its fields and generic params.
// ─────────────────────────────────────────────────────────────────────────────

struct StructDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::StructDecl;

    std::string name;
    std::vector<GenericParamPtr> genericParams; // empty if non-generic
    std::vector<FieldDeclPtr> fields;
    Visibility visibility = Visibility::Private;

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

    std::string name;
    std::optional<int>
        explicitValue; // only present when '= INT_LITERAL' was written

    explicit EnumVariantAST(std::string n)
        : BaseAST(ASTKind::EnumVariant), name(std::move(n)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using EnumVariantPtr = std::unique_ptr<EnumVariantAST>;

// ─────────────────────────────────────────────────────────────────────────────
// EnumDeclAST
//
// A named, closed set of integer-backed constants.
//   pub enum Direction { North  South  East  West }
//   pub enum ShaderStage { Vertex = 0x01  Fragment = 0x02  Compute = 0x04 }
//
// The semantic pass chooses the backing integer type:
//   - byte  (int8)  for ≤ 255 variants
//   - short (int16) for more
// Variants are accessed via EnumName.Variant dot syntax — never bare names.
// ─────────────────────────────────────────────────────────────────────────────

struct EnumDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::EnumDecl;

    std::string name;
    std::vector<EnumVariantPtr> variants;
    Visibility visibility = Visibility::Private;

    EnumDeclAST() : DeclAST(ASTKind::EnumDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TraitMethodAST
//
// A single method signature inside a trait — no body, no `=`.
//   draw   ()
//   bounds () Rect
//   compareTo (other T) int
//
// This is not a MethodDeclAST because it has no body. The semantic pass uses
// this node when checking that ImplDeclAST provides every required method.
// returnType is nullptr for void methods.
// ─────────────────────────────────────────────────────────────────────────────

struct TraitMethodAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitMethod;

    std::string name;
    std::vector<ParamPtr> params;
    TypePtr returnType; // nullptr = void

    TraitMethodAST() : BaseAST(ASTKind::TraitMethod) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using TraitMethodPtr = std::unique_ptr<TraitMethodAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TraitDeclAST
//
// A named method contract — signatures only, no bodies, no fields.
//   pub trait Drawable { draw ()  bounds () Rect }
//   pub trait Comparable<T> { compareTo (other T) int }
//
// Used by the semantic pass to verify impl conformance and as a constraint
// in generic param declarations (T : Drawable).
// ─────────────────────────────────────────────────────────────────────────────

struct TraitDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TraitDecl;

    std::string name;
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
// Not a DeclAST — it is a helper owned by ImplDeclAST.
// The semantic pass resolves name to a TraitDeclAST and checks that
// genericArgs count matches the trait's generic params.
// ─────────────────────────────────────────────────────────────────────────────

struct TraitRefAST {
    std::string name;   // trait name, e.g. "Comparable"
    std::vector<TypePtr>
        genericArgs;    // concrete args, e.g. [Int] for Comparable<int>

    SourceLocation loc; // for error reporting
};

using TraitRefPtr = std::unique_ptr<TraitRefAST>;

// ─────────────────────────────────────────────────────────────────────────────
// MethodDeclAST
//
// A method body inside an impl block.
//   length () float = { return (x*x + y*y) -> sqrt }
//   dot (other Vec2) float = { return x*other.x + y*other.y }
//   drawAll () = { for obj in objects { obj.draw() } }
//
// No per-method visibility prefix — visibility is set at the ImplDeclAST level
// (pub impl = public methods, bare impl = package-private methods).
// returnType is nullptr for void methods.
// isAsync is true when the body was written as async { ... }.
// ─────────────────────────────────────────────────────────────────────────────

struct MethodDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MethodDecl;

    std::string name;
    std::vector<ParamPtr> params;
    TypePtr returnType; // nullptr = void
    StmtPtr body;       // BlockStmtAST
    FuncBodyKind bodyKind = FuncBodyKind::Block;
    bool isAsync = false;

    MethodDeclAST() : BaseAST(ASTKind::MethodDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using MethodDeclPtr = std::unique_ptr<MethodDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromDeclAST
//
// A type conversion declaration inside a pub impl block.
//   from (c Celsius) Fahrenheit = { return Fahrenheit { value = c.value * 9/5 + 32 } }
//
// Only valid inside pub impl — bare impl cannot declare `from`.
// Enables TypeName(expr) call syntax at use sites:
//   let f Fahrenheit = Fahrenheit(boiling)   ← calls this from declaration
//
// Multiple from declarations are allowed on the same type, each with a
// different source parameter type. The semantic pass checks for duplicates.
//
// srcParamName — the name of the source parameter ("c" in the example)
// srcParamType — the source type ("Celsius")
// returnTypeName — must match the enclosing impl's struct name
// ─────────────────────────────────────────────────────────────────────────────

struct FromDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FromDecl;

    std::string srcParamName;   // "c"
    TypePtr srcParamType;       // Celsius
    std::string returnTypeName; // "Fahrenheit" — semantic pass verifies matches
                                // impl target
    StmtPtr body;               // BlockStmtAST

    FromDeclAST() : BaseAST(ASTKind::FromDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using FromDeclPtr = std::unique_ptr<FromDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// ImplDeclAST
//
// Binds methods (and from conversions) to a struct type.
//   pub impl Vec2 { ... }
//   pub impl Circle : Drawable { ... }
//   pub impl<T : Drawable> Scene<T> { ... }
//   impl Vec2 { ... }   -- package-private methods
//
// isPub — true = methods callable by anyone holding the value
//         false = methods callable only within the package
//
// genericParams — type params on the impl itself, e.g. <T : Drawable>
//
// structName — the name of the type being implemented ("Vec2", "Scene")
//
// structGenericArgs — the type args supplied to the struct name, e.g. the
//   <T> in  impl<T : Drawable> Scene<T>. These bind the generic param T
//   declared above to the struct's type parameter.
//
// traitRef — present when ": TraitName" was written; nullptr otherwise.
//   The semantic pass uses this to verify full trait conformance.
//
// methods — regular method bodies
// fromDecls — type conversion declarations (pub impl only)
//
// Multiple impl blocks for the same struct merge at semantic time.
// ─────────────────────────────────────────────────────────────────────────────

struct ImplDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ImplDecl;

    Visibility visibility = Visibility::Private;
    std::vector<GenericParamPtr> genericParams; // impl-level type params
    std::string structName;                     // "Vec2", "Scene"
    std::vector<TypePtr> structGenericArgs;     // <T> in Scene<T>
    TraitRefPtr traitRef;                       // nullptr if no conformance
    std::vector<MethodDeclPtr> methods;
    std::vector<FromDeclPtr> fromDecls;         // only valid when isPub

    ImplDeclAST() : DeclAST(ASTKind::ImplDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeAliasDeclAST
//
// A named alias for an existing type shape.
//   type ID          = int
//   type Number      = int | float
//   type Callback    = (event Event) bool
//   type Transform<T> = (value T) T
//
// Does not create a new nominal type — ID and int are interchangeable at the
// semantic level. Use struct for a distinct nominal type.
// aliasedType holds the full TypeAST on the right-hand side.
// ─────────────────────────────────────────────────────────────────────────────

struct TypeAliasDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeAliasDecl;

    std::string name;
    std::vector<GenericParamPtr> genericParams; // empty if non-generic
    TypePtr aliasedType;                        // the right-hand side
    Visibility visibility = Visibility::Private;

    TypeAliasDeclAST() : DeclAST(ASTKind::TypeAliasDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ExternDeclAST
//
// An external C / Vulkan symbol declaration — body is absent.
//   extern let malloc (size uint64) @uint8
//   extern let vkCreateInstance (pInfo @VkInstanceCreateInfo ...) uint32
//
// The `extern` modifier signals to codegen that no body will be generated —
// the linker resolves the symbol from a C/Vulkan library.
//
// Raw pointer types (@T) are only valid inside extern declarations.
// The semantic pass enforces this by checking that PtrTypeAST nodes appear
// only as children of ExternDeclAST subtrees.
//
// params is a flat list (no curry groups — extern functions are never curried).
// returnType nullptr = void.
// ─────────────────────────────────────────────────────────────────────────────

struct ExternDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ExternDecl;

    std::string name;
    std::vector<ParamPtr> params;
    TypePtr returnType; // nullptr = void

    ExternDeclAST() : DeclAST(ASTKind::ExternDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};