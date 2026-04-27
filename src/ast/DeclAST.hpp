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
//   VarDeclAST          — let / const  name  type  [= expr]
//   ParamAST            — name type  or  name ...type  (parameter of a function)
//   GenericParamAST     — T  or  T : Trait  or  T : A + B 
//   FuncDeclAST         — let/const  name  [<generics>]  (group)+ [returnType]  = body
//   StructDeclAST       — [Visibility] struct Name [<generics>] { fields }
//   FieldDeclAST        — name type [= defaultExpr]
//   EnumDeclAST         — [Visibility] enum Name { variants }
//   EnumVariantAST      — VariantName [= INT_LITERAL]
//   TraitDeclAST        — [Visibility] trait Name [<generics>] { method signatures }
//   TraitMethodAST      — name (params) [returnType]   (signature only, no body)
//   ImplDeclAST         — [Visibility] impl [<generics>] Name [: TraitRef] { members }
//   TraitRefAST         — TraitName [<genericArgs>]    (the ": Drawable" part)
//   MethodDeclAST       — name (params) [returnType] = body
//   FromDeclAST         — from (paramName paramType) returnType = body
//   TypeAliasDeclAST    — [Visibility] type Name [<generics>] = TypeAST
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// AttributeAST
//
// A compiler directive annotation attached to a declaration.
// Use @extern("name") on a let/const declaration to bind a C/Vulkan symbol.
//
// Forms:
//   @extern("malloc")                  — FFI symbol name
//   @extern("vkCreateInstance", "C")   — FFI with calling convention
//   @inline                            — inlining hint
//   @noinline                          — prevent inlining
//   @packed                            — remove struct padding
//   @deprecated("Use newFunc instead") — deprecation warning
//
// Parameters are restricted to string literals, integer literals, booleans, and
// type identifiers — no runtime expressions inside attributes.
// ─────────────────────────────────────────────────────────────────────────────

// AttributeArg — one argument inside an attribute's parentheses.
// Valid forms:  "string"  |  42  |  true/false  |  TypeName
struct AttributeArgAST {
    // Which kind of argument this is.
    enum class ArgKind { StringLit, IntLit, BoolLit, TypeIdent };
    ArgKind     argKind;
    std::string value;   // raw string for StringLit/IntLit/TypeIdent; "true"/"false" for Bool
    SourceLocation loc;
};

struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    std::string name;                               // "extern", "inline", "packed", "deprecated", …
    std::vector<AttributeArgAST> args;              // may be empty when no () follows
    SourceLocation loc;

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using AttributePtr = std::unique_ptr<AttributeAST>;

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
// FuncBodyKind — which syntactic form was used for the function body.
// The parser sets this so the semantic pass knows what it is walking.
//
//   Block    — let f (x int) int = { return x + 1 }
//   AnonFunc — let f (x int) int = (x int) int { return x + 1 }   (verbose form)
//   ExprBody — let f (x int) = existingFunc   (function/expression assignment)
//
// Note: match-as-body and if-as-body are sugar — the parser desugars them
// into a BlockStmtAST containing a single MatchExprAST / IfExprAST statement
// before storing, so the AST always holds a block. FuncBodyKind::Block covers
// all three of those forms from the parser's perspective.
// ExprBody is also desugared into a BlockStmtAST containing a ReturnStmtAST.
// async bodies are indicated by the isAsync flag on FuncDeclAST /
// MethodDeclAST.
// ─────────────────────────────────────────────────────────────────────────────

enum class FuncBodyKind {
    Block,    // standard expr_block body: = { ... }
    AnonFunc, // explicit anonymous function: = (params) ret { ... }
    ExprBody, // function/expression assignment: = expression
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
    std::string name;
    TypePtr type;        // always present — annotation is required
    ExprPtr init;        // nullptr if no initialiser was written
    Visibility visibility = Visibility::Private;
    std::vector<AttributePtr> attributes;  // @extern, @inline, etc. — may be empty

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
//   let   add (a int) (b int) int    = { return a + b }
//   const greet (name string)        = { io.printl("hi " + name) }
//   let   fetch (url string) string  = async { return await httpGet(url) }
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
    TypePtr signature;                              // Synthesized Function Type (signature)
    Visibility visibility = Visibility::Private;
    std::vector<AttributePtr> attributes;           // @extern("sym"), @inline, etc.

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

    std::string name;
    std::vector<GenericParamPtr> genericParams; // empty if non-generic
    std::vector<FieldDeclPtr> fields;
    Visibility visibility = Visibility::Private;
    std::vector<AttributePtr> attributes;       // @packed, @deprecated, etc.
    
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
    mutable std::unique_ptr<NamedTypeAST> selfType;

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
//   clamp (min int) (max int) int    -- curried trait method signature
//
// paramGroups mirrors FuncDeclAST: outer = curry groups, inner = params.
// Single group = normal method; multiple groups = curried method signature.
// This is not a MethodDeclAST because it has no body. The semantic pass uses
// this node when checking that ImplDeclAST provides every required method.
// returnType is nullptr for void methods.
// ─────────────────────────────────────────────────────────────────────────────

struct TraitMethodAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitMethod;

    std::string name;
    std::vector<std::vector<ParamPtr>> paramGroups; // outer = curry groups
    TypePtr returnType; // nullptr = void
    bool isAsync = false;
    TypePtr signature;                              // Synthesized Function Type (signature)

    TraitMethodAST() : BaseAST(ASTKind::TraitMethod) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using TraitMethodPtr = std::unique_ptr<TraitMethodAST>;

// ─────────────────────────────────────────────────────────────────────────────
// TraitDeclAST
//
// A named method contract — signatures only, no bodies, no fields.
//   Visibility trait Drawable { draw ()  bounds () Rect }
//   Visibility trait Comparable<T> { compareTo (other T) int }
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
//   clamp (min int) (max int) (value int) int = { ... }   -- curried method
//
// paramGroups mirrors FuncDeclAST: outer = curry groups, inner = params.
// Single group = normal method; multiple groups = curried method.
// No per-method visibility prefix — visibility is set at the ImplDeclAST level.
// returnType is nullptr for void methods.
// isAsync is true when the body was written as async { ... }.
// ─────────────────────────────────────────────────────────────────────────────

struct MethodDeclAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::MethodDecl;

    std::string name;
    std::vector<std::vector<ParamPtr>> paramGroups; // outer = curry groups
    TypePtr returnType; // nullptr = void
    StmtPtr body;       // BlockStmtAST
    FuncBodyKind bodyKind = FuncBodyKind::Block;
    bool isAsync = false;
    TypePtr signature;                              // Synthesized Function Type (signature)

    MethodDeclAST() : BaseAST(ASTKind::MethodDecl) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using MethodDeclPtr = std::unique_ptr<MethodDeclAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromEntryAST
//
// A single conversion entry inside a from block.
//   celsius (c Celsius) Fahrenheit = { ... }
//   celsius (c Celsius) (scale float) Fahrenheit = { ... }   -- curried
//
// paramGroups mirrors FuncDeclAST: outer = curry groups, inner = params.
// Single group = normal conversion; multiple groups = curried conversion.
// returnTypeName must match the enclosing FromDeclAST's targetTypeName —
// enforced by the semantic pass.
// ─────────────────────────────────────────────────────────────────────────────
struct FromEntryAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::FromEntry;

    std::vector<std::vector<ParamPtr>> paramGroups; // outer = curry groups
    std::string returnTypeName; // "Fahrenheit"
    StmtPtr     body;           // BlockStmtAST
    FuncBodyKind bodyKind = FuncBodyKind::Block;

    FromEntryAST() : BaseAST(ASTKind::FromEntry) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using FromEntryPtr = std::unique_ptr<FromEntryAST>;

// ─────────────────────────────────────────────────────────────────────────────
// FromDeclAST
//
// A top-level type conversion block.
//   export from Fahrenheit {
//       celsius (c Celsius) Fahrenheit = { ... }
//   }
//
// Multiple from declarations are allowed, each with a different source parameter type.
// ─────────────────────────────────────────────────────────────────────────────

struct FromDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::FromDecl;

    Visibility visibility = Visibility::Private;
    std::string targetTypeName; // "Fahrenheit"
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

    Visibility visibility = Visibility::Private;
    std::vector<GenericParamPtr> genericParams; // impl-level type params
    std::string structName;                     // "Vec2", "Scene"
    std::vector<TypePtr> structGenericArgs;     // <T> in Scene<T>
    TraitRefPtr traitRef;                       // nullptr if no conformance
    std::vector<MethodDeclPtr> methods;

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