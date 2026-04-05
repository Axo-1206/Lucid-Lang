/**
 * @file BaseAST.hpp
 *
 * @responsibility The Foundation. Defines the BaseAST, the Visitor interface, and common types (DocComment, SourceLocation).
 *
 * @architectural_note 
 *   This file uses Forward Declarations for all AST families (Expr, Stmt, etc.). 
 *   NEVER include a family header (like ExprAST.hpp) here; this keeps the dependency graph acyclic.
 *
 * @related_files 
 *   - src/ast/ExprAST.hpp, StmtAST.hpp, DeclAST.hpp, TypeAST.hpp, (Concrete implementations)
 */

#pragma once

#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <cassert>   // required by BaseAST::as<T>()

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations — every AST family forward-declared here so any header
// can accept a visitor or hold a pointer without pulling in the full family.
//
// IMPORTANT: these are forward declarations only — no fields, no bodies.
// The actual struct definitions live in their own headers:
//
//   TypeAST.hpp     — PrimitiveTypeAST, NamedTypeAST, FixedArrayTypeAST, ...
//   DeclAST.hpp     — FuncDeclAST, StructDeclAST, ImplDeclAST, ...
//   ExprAST.hpp     — LiteralExprAST, CallExprAST, PipelineExprAST, ...
//   StmtAST.hpp     — BlockStmtAST, ForStmtAST, ParallelForStmtAST, ...
//
// BaseAST.hpp includes none of them — this keeps the include graph acyclic.
// Each family header does:  #include "BaseAST.hpp"
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// ASTKind — compile-time tag stored on every node.
//
// Replaces runtime RTTI / dynamic_cast with a single integer comparison.
// Pattern (LLVM-style):
//
//   if (node->kind == ASTKind::PrimitiveType) {
//       auto* p = static_cast<PrimitiveTypeAST*>(node);
//   }
//
// Or use the helpers on BaseAST:
//   if (node->isa<PrimitiveTypeAST>())  { ... node->as<PrimitiveTypeAST>() ... }
//
// Every concrete node constructor passes its staticKind up to BaseAST(kind).
// The five thin family bases (TypeAST, DeclAST, …) forward kind upward too.
// ─────────────────────────────────────────────────────────────────────────────

enum class ASTKind : uint16_t {
    // ── Type nodes ────────────────────────────────────────────────────────────
    PrimitiveType,
    NamedType,
    NullableType,
    UnionType,
    FixedArrayType,
    SliceType,
    DynamicArrayType,
    RefType,
    PtrType,
    FuncType,

    // ── Declaration nodes ─────────────────────────────────────────────────────
    PackageDecl,
    UseDecl,
    ModuleDecl,
    VarDecl,
    Param,
    GenericParam,
    FuncDecl,
    FieldDecl,
    StructDecl,
    EnumVariant,
    EnumDecl,
    TraitMethod,
    TraitDecl,
    MethodDecl,
    FromDecl,
    ImplDecl,
    TypeAliasDecl,
    ExternDecl,

    // ── Expression nodes ──────────────────────────────────────────────────────
    LiteralExpr,
    ArrayLiteralExpr,
    StructLiteralExpr,
    IdentifierExpr,
    FieldAccessExpr,
    BehaviorAccessExpr,
    CallExpr,
    IndexExpr,
    BinaryExpr,
    UnaryExpr,
    AssignExpr,
    IsExpr,
    NullableChainExpr,
    PipelineExpr,
    ComposeExpr,
    AnonFuncExpr,
    AwaitExpr,
    MatchExpr,
    IfExpr,             // IfInlineExprAST — ?? sugar form
    RangeExpr,
    TypeConvExpr,

    // ── Statement nodes ───────────────────────────────────────────────────────
    BlockStmt,
    ExprStmt,
    DeclStmt,
    IfStmt,
    SwitchStmt,
    ForStmt,
    WhileStmt,
    DoWhileStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    ParallelForStmt,
    ParallelBlockStmt,

    // ── Pattern nodes ─────────────────────────────────────────────────────────
    // Note: literal and range patterns are not listed here — they reuse
    // LiteralExpr and RangeExpr directly in pattern position.
    BindPattern,
    WildcardPattern,
    TypePattern,
    StructPattern,
    MatchArm,
    DefaultArm,

    // ── Root ──────────────────────────────────────────────────────────────────
    Program,
};

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct NullableTypeAST;
struct UnionTypeAST;
struct FixedArrayTypeAST;    // [N]T   — compile-time size, stack/inline
struct SliceTypeAST;         // []T    — fat-pointer view, no ownership
struct DynamicArrayTypeAST;  // [*]T   — heap-owned, growable
struct RefTypeAST;
struct PtrTypeAST;
struct FuncTypeAST;

// DeclAST.hpp
struct PackageDeclAST;
struct UseDeclAST;
struct ModuleDeclAST;
struct VarDeclAST;
struct ParamAST;
struct GenericParamAST;
struct FuncDeclAST;
struct FieldDeclAST;
struct StructDeclAST;
struct EnumVariantAST;      // the data inside the enum definition
struct EnumDeclAST;         // enum definition
struct TraitMethodAST;
struct TraitDeclAST;
struct MethodDeclAST;
struct FromDeclAST;         // from [method definition] - use for type convertion
struct ImplDeclAST;
struct TypeAliasDeclAST;
struct ExternDeclAST;

// ExprAST.hpp
struct LiteralExprAST;
struct IdentifierExprAST;
struct ArrayLiteralExprAST;
struct StructLiteralExprAST;
struct BinaryExprAST;
struct UnaryExprAST;
struct CallExprAST;
struct IndexExprAST;
struct FieldAccessExprAST;
struct BehaviorAccessExprAST;
struct NullableChainExprAST;
struct AssignExprAST;
struct IsExprAST;
struct PipelineExprAST;
struct ComposeExprAST;
struct AnonFuncExprAST;
struct AwaitExprAST;
struct MatchExprAST;
struct IfExprAST;
struct RangeExprAST;
struct TypeConvExprAST;

// Pattern nodes (defined in ExprAST.hpp alongside MatchArmAST / DefaultArmAST)
// LiteralPatternAST and RangePatternAST are removed — LiteralExprAST and
// RangeExprAST are used directly in pattern position inside MatchArmAST::patterns.
struct BindPatternAST;
struct WildcardPatternAST;
struct TypePatternAST;
struct StructPatternAST;
struct MatchArmAST;
struct DefaultArmAST;

// StmtAST.hpp
struct BlockStmtAST;
struct ExprStmtAST;
struct DeclStmtAST;
struct IfStmtAST;
struct SwitchStmtAST;
struct ForStmtAST;
struct WhileStmtAST;
struct DoWhileStmtAST;
struct ReturnStmtAST;
struct BreakStmtAST;
struct ContinueStmtAST;
struct ParallelForStmtAST;
struct ParallelBlockStmtAST;

// Root
struct ProgramAST;

// ─────────────────────────────────────────────────────────────────────────────
// IndexKind — distinguishes the two postfix index operations on arrays.
//
//   Element  — nums[i]        single element access, produces T
//   Slice    — nums[i..j]     inclusive range access, produces []T (SliceTypeAST)
//
// Both parse as postfix_op on the same target expression but produce different
// types, require different semantic validation, and have different codegen paths.
// Kept here so ExprAST.hpp and the semantic pass can use it without any extra
// include — IndexExprAST holds one of these to distinguish the two forms.
// ─────────────────────────────────────────────────────────────────────────────

enum class IndexKind {
    Element,  // expr '[' expr ']'              — nums[2]
    Slice,    // expr '[' expr '..' expr ']'    — nums[1..3]  (inclusive both ends)
};

// ─────────────────────────────────────────────────────────────────────────────
// DocComment
//
// All three grammar forms collapse into a single string of Markdown text.
// The parser strips the ' -' line prefix from block form before storing.
// The form tag lets the LSP and doc generator know how the comment was written,
// which affects attachment priority (stacked > block > trailing).
//
// Attachment rules (from LUC_GRAMMAR.md):
//   Stacked  — consecutive '--' lines immediately above the declaration
//   Block    — /-- ... --/ immediately above the declaration
//   Trailing — '--' comment on the same line as the declaration
//   Stacked + trailing  → stacked wins, trailing ignored
//   Block    + stacked  → block attaches, stacked above it is floating
// ─────────────────────────────────────────────────────────────────────────────

enum class DocCommentForm {
    Stacked,   // -- line one \n -- line two  (above declaration)
    Block,     // /-- ... --/                 (above declaration)
    Trailing,  // let x int = 5 -- inline doc (same line)
};

struct DocComment {
    std::string     text;   // Markdown content, ' -' prefix already stripped
    DocCommentForm  form;
};

// ─────────────────────────────────────────────────────────────────────────────
// SourceLocation
//
// Line and column are 1-based, matching the Lexer's Token.line / Token.column.
// file is the path of the source file relative to the package root — this
// matters once the semantic pass resolves cross-file imports.
// ─────────────────────────────────────────────────────────────────────────────

struct SourceLocation {
    int         line   = 0;
    int         column = 0;
    std::string file;           // relative path, e.g. "math/vec2.luc"

    // Convenience — a default-constructed SourceLocation is "unknown".
    bool isKnown() const { return line > 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Visitor — one virtual visit() per concrete node type.
//
// Every pass (semantic, codegen, printer) subclasses ASTVisitor and overrides
// the methods it cares about. The default implementation does nothing, so a
// pass only overrides what it needs.
//
// Usage:
//   node->accept(myVisitor);   // dispatches to the correct visit() override
// ─────────────────────────────────────────────────────────────────────────────

struct ASTVisitor {

    virtual ~ASTVisitor() = default;

    // ── Type nodes ────────────────────────────────────────────────────────────
    virtual void visit(PrimitiveTypeAST&)      {}
    virtual void visit(NamedTypeAST&)          {}
    virtual void visit(NullableTypeAST&)       {}
    virtual void visit(UnionTypeAST&)          {}
    virtual void visit(FixedArrayTypeAST&)     {}   // [N]T
    virtual void visit(SliceTypeAST&)          {}   // []T
    virtual void visit(DynamicArrayTypeAST&)   {}   // [*]T
    virtual void visit(RefTypeAST&)            {}
    virtual void visit(PtrTypeAST&)            {}
    virtual void visit(FuncTypeAST&)           {}

    // ── Declaration nodes ─────────────────────────────────────────────────────
    virtual void visit(PackageDeclAST&)     {}
    virtual void visit(UseDeclAST&)         {}
    virtual void visit(ModuleDeclAST&)      {}
    virtual void visit(VarDeclAST&)         {}
    virtual void visit(FuncDeclAST&)        {}
    virtual void visit(StructDeclAST&)      {}
    virtual void visit(FieldDeclAST&)       {}
    virtual void visit(EnumDeclAST&)        {}
    virtual void visit(EnumVariantAST&)     {}
    virtual void visit(TraitMethodAST&)     {}
    virtual void visit(TraitDeclAST&)       {}
    virtual void visit(ImplDeclAST&)        {}
    virtual void visit(MethodDeclAST&)      {}
    virtual void visit(FromDeclAST&)        {}
    virtual void visit(TypeAliasDeclAST&)   {}
    virtual void visit(ParamAST&)           {}
    virtual void visit(GenericParamAST&)    {}
    virtual void visit(ExternDeclAST&)    	{}
    

    // ── Expression nodes ──────────────────────────────────────────────────────
    virtual void visit(LiteralExprAST&)         {}
    virtual void visit(IdentifierExprAST&)      {}
    virtual void visit(ArrayLiteralExprAST&)    {}
    virtual void visit(StructLiteralExprAST&)   {}
    virtual void visit(BinaryExprAST&)          {}
    virtual void visit(UnaryExprAST&)           {}
    virtual void visit(CallExprAST&)            {}
    virtual void visit(IndexExprAST&)           {}
    virtual void visit(FieldAccessExprAST&)     {}
    virtual void visit(BehaviorAccessExprAST&)  {}
    virtual void visit(NullableChainExprAST&)   {}
    virtual void visit(AssignExprAST&)          {}
    virtual void visit(IsExprAST&)              {}
    virtual void visit(PipelineExprAST&)        {}
    virtual void visit(ComposeExprAST&)         {}
    virtual void visit(AnonFuncExprAST&)        {}
    virtual void visit(AwaitExprAST&)           {}
    virtual void visit(MatchExprAST&)           {}
    virtual void visit(IfExprAST&)              {} 
    virtual void visit(RangeExprAST&)           {}
    virtual void visit(TypeConvExprAST&)        {}

    // ── Pattern nodes ─────────────────────────────────────────────────────────
    // No visit() for LiteralExprAST or RangeExprAST in pattern position —
    // those are dispatched through the existing expression visit() overrides above.
    virtual void visit(BindPatternAST&)         {}
    virtual void visit(WildcardPatternAST&)     {}
    virtual void visit(TypePatternAST&)         {}
    virtual void visit(StructPatternAST&)       {}
    virtual void visit(MatchArmAST&)            {}
    virtual void visit(DefaultArmAST&)          {}

    // ── Statement nodes ───────────────────────────────────────────────────────
    virtual void visit(BlockStmtAST&)           {}
    virtual void visit(ExprStmtAST&)            {}
    virtual void visit(DeclStmtAST&)            {}
    virtual void visit(IfStmtAST&)              {}
    virtual void visit(SwitchStmtAST&)          {}
    virtual void visit(ForStmtAST&)             {}
    virtual void visit(WhileStmtAST&)           {}
    virtual void visit(DoWhileStmtAST&)         {}
    virtual void visit(ReturnStmtAST&)          {}
    virtual void visit(BreakStmtAST&)           {}
    virtual void visit(ContinueStmtAST&)        {}
    virtual void visit(ParallelForStmtAST&)     {}
    virtual void visit(ParallelBlockStmtAST&)   {}

    // ── Root ──────────────────────────────────────────────────────────────────
    virtual void visit(ProgramAST&)             {}
};

// ─────────────────────────────────────────────────────────────────────────────
// BaseAST
//
// Root of the entire node hierarchy. Every concrete AST node inherits from
// BaseAST (directly or through a thin family base — see below).
//
// Fields
//   loc        — where in source this node begins (set by the parser)
//   doc        — documentation comment attached to this node, if any
//                (always stored regardless of visibility — grammar rule)
//
// Semantic fields (written by the semantic pass, read by codegen)
//   resolvedType     — the type this node evaluates to, after type checking
//   isBehaviorMember — true when this node is a Type:method reference;
//                      the semantic pass uses this to block reassignment
//   isConst          — true when the value is known at compile time
//   scopeDepth       — nesting depth of the enclosing scope (0 = top-level)
//
// These fields start null / false / 0 and are filled in during the semantic
// phase. Codegen should never read them before semantic has run.
// ─────────────────────────────────────────────────────────────────────────────

struct BaseAST {

    // ── Node kind (LLVM-style fast discrimination) ────────────────────────────
    // Set once by every concrete node constructor — never changes after that.
    // Use isa<T>() / as<T>() helpers below instead of dynamic_cast.
    ASTKind kind;

    // ── Source location ───────────────────────────────────────────────────────
    SourceLocation              loc;

    // ── Documentation ─────────────────────────────────────────────────────────
    // Grammar: "always store, always show in LSP, filter by visibility only at
    // doc generation time." — storing here satisfies that rule universally.
    std::optional<DocComment>   doc;

    // ── Semantic annotations (written by semantic pass) ───────────────────────
    // Forward-declared as void* so BaseAST.hpp has zero dependency on TypeAST.
    // The semantic pass casts this to the correct TypeAST* after it resolves it.
    void*   resolvedType     = nullptr;  // TypeAST* — type of this node
    bool    isBehaviorMember = false;    // true → Type:method, never reassignable
    bool    isConst          = false;    // true → value is compile-time constant
    int     scopeDepth       = 0;        // 0 = file scope, +1 per nested block

    // ── Constructor ───────────────────────────────────────────────────────────
    explicit BaseAST(ASTKind k) : kind(k) {}

    // ── Virtual destructor ────────────────────────────────────────────────────
    virtual ~BaseAST() = default;

    // ── Visitor dispatch ──────────────────────────────────────────────────────
    // Every concrete node overrides this with: visitor.visit(*this);
    virtual void accept(ASTVisitor& visitor) = 0;

    // ── Kind-based helpers (zero-overhead alternatives to dynamic_cast) ────────
    //
    // isa<T>()  — returns true if this node is exactly of type T
    // as<T>()   — static downcast; debug-asserts kind matches, no check in release
    //
    // Usage:
    //   if (node->isa<PrimitiveTypeAST>()) {
    //       auto* p = node->as<PrimitiveTypeAST>();
    //   }
    template<typename T>
    bool isa() const { return kind == T::staticKind; }

    template<typename T>
    T* as() {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>() cast");
        return static_cast<T*>(this);
    }

    template<typename T>
    const T* as() const {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>() cast");
        return static_cast<const T*>(this);
    }

    // ── Convenience ───────────────────────────────────────────────────────────
    bool hasDoc()  const { return doc.has_value(); }
    bool hasType() const { return resolvedType != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Thin family bases
//
// These add no data — they exist purely so the parser and semantic pass can
// hold a heterogeneous list of "any type node" or "any expression node" via
// a single pointer type, without knowing the concrete subtype.
//
//   TypeAST*    — any type node    (PrimitiveTypeAST, FixedArrayTypeAST, ...)
//   DeclAST*    — any declaration  (FuncDeclAST, StructDeclAST, ...)
//   ExprAST*    — any expression   (LiteralExprAST, CallExprAST, ...)
//   StmtAST*    — any statement    (BlockStmtAST, ForStmtAST, ...)
//   PatternAST* — any pattern      (BindPatternAST, StructPatternAST, ...)
//
// Usage in the parser:
//   std::unique_ptr<ExprAST> expr = parseExpr();
//   std::vector<std::unique_ptr<StmtAST>> stmts;
// ─────────────────────────────────────────────────────────────────────────────

// Each family base forwards the kind tag up to BaseAST — concrete nodes
// pass their staticKind through these constructors.
struct TypeAST    : BaseAST { explicit TypeAST(ASTKind k)    : BaseAST(k) {} };
struct DeclAST    : BaseAST { explicit DeclAST(ASTKind k)    : BaseAST(k) {} };
struct ExprAST    : BaseAST { explicit ExprAST(ASTKind k)    : BaseAST(k) {} };
struct StmtAST    : BaseAST { explicit StmtAST(ASTKind k)    : BaseAST(k) {} };
struct PatternAST : BaseAST { explicit PatternAST(ASTKind k) : BaseAST(k) {} };

// ─────────────────────────────────────────────────────────────────────────────
// Ownership helpers
//
// The AST owns all its nodes via unique_ptr. These aliases keep declarations
// readable — the family headers use them exclusively.
// ─────────────────────────────────────────────────────────────────────────────

using TypePtr    = std::unique_ptr<TypeAST>;
using DeclPtr    = std::unique_ptr<DeclAST>;
using ExprPtr    = std::unique_ptr<ExprAST>;
using StmtPtr    = std::unique_ptr<StmtAST>;
using PatternPtr = std::unique_ptr<PatternAST>;

// ─────────────────────────────────────────────────────────────────────────────
// ProgramAST — root of the entire tree
//
// The parser produces exactly one ProgramAST per source file.
// The semantic pass walks all files' ProgramASTs to resolve imports.
// ─────────────────────────────────────────────────────────────────────────────

struct ProgramAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    std::string            packageName;   // from `package foo`
    std::string            filePath;      // relative path, e.g. "math/vec2.luc"
    std::vector<DeclPtr>   decls;         // top-level declarations in order

    ProgramAST() : BaseAST(ASTKind::Program) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(*this); }
};