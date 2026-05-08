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

#include "debug/DebugMacros.hpp"
#include "support/ASTArena.hpp"

#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <cassert>

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
#include "support/InternedString.hpp"
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
    Unknown,
    UnknownDecl,
    UnknownExpr,
    UnknownStmt,
    UnknownType,

    // ── Type nodes ────────────────────────────────────────────────────────────
    PrimitiveType,
    NamedType,
    NullableType,
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
    TraitRef,
    MethodDecl,
    FromDecl,
    FromEntry,
    ImplDecl,
    TypeAliasDecl,

    // ── Expression nodes ──────────────────────────────────────────────────────
    LiteralExpr,
    ArrayLiteralExpr,
    StructLiteralExpr,
    FieldInit,
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
    NullCoalesceExpr,
    PipelineExpr,
    PipelineStep,
    ComposeExpr,
    ComposeOperand,
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
    SwitchCase,
    ForStmt,
    WhileStmt,
    DoWhileStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    ParallelForStmt,
    ParallelBlockStmt,

    // ── Pattern nodes ─────────────────────────────────────────────────────────
    BindPattern,
    WildcardPattern,
    TypePattern,
    StructPattern,
    FieldPattern,
    PatternExpr,
    MatchArm,
    DefaultArm,

    // ── Root ──────────────────────────────────────────────────────────────────
    Program,

    // ── Compiler Directives (@) ───────────────────────────────────────────────
    Attribute,          // @extern("name"), @inline, @packed, @deprecated("msg")
    AttributeArg,       // argument inside an attribute's parentheses
    IntrinsicCallExpr,  // @sizeof(T), @memcpy(dest, src, len), @sqrt(x)
};

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct NullableTypeAST;
struct FixedArrayTypeAST;    // [N]T   — compile-time size, stack/inline
struct SliceTypeAST;         // []T    — fat-pointer view, no ownership
struct DynamicArrayTypeAST;  // [*]T   — heap-owned, growable
struct RefTypeAST;
struct PtrTypeAST;
struct FuncTypeAST;

// DeclAST.hpp
struct PackageDeclAST;
struct UseDeclAST;
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
struct TraitRefAST;         // trait reference in impl conformance
struct MethodDeclAST;
struct FromDeclAST;         // from [method definition] - use for type casting
struct FromEntryAST;        // entry inside the from block
struct ImplDeclAST;
struct TypeAliasDeclAST;

// ExprAST.hpp
struct LiteralExprAST;
struct IdentifierExprAST;
struct ArrayLiteralExprAST;
struct StructLiteralExprAST;
struct FieldInitAST;
struct BinaryExprAST;
struct UnaryExprAST;
struct CallExprAST;
struct IndexExprAST;
struct FieldAccessExprAST;
struct BehaviorAccessExprAST;
struct NullableChainExprAST;
struct NullCoalesceExprAST;
struct AssignExprAST;
struct IsExprAST;
struct PipelineExprAST;
struct PipelineStepAST;
struct ComposeExprAST;
struct ComposeOperandAST;
struct AnonFuncExprAST;
struct AwaitExprAST;
struct MatchExprAST;
struct IfExprAST;
struct RangeExprAST;
struct TypeConvExprAST;

// Pattern nodes (defined in ExprAST.hpp alongside MatchArmAST / DefaultArmAST)
struct BindPatternAST;
struct WildcardPatternAST;
struct TypePatternAST;
struct StructPatternAST;
struct FieldPatternAST;
struct PatternExprAST;
struct MatchArmAST;
struct DefaultArmAST;

// StmtAST.hpp
struct BlockStmtAST;
struct ExprStmtAST;
struct DeclStmtAST;
struct IfStmtAST;
struct SwitchStmtAST;
struct SwitchCaseAST;
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

// Unknown nodes
struct UnknownDeclAST;
struct UnknownExprAST;
struct UnknownStmtAST;
struct UnknownTypeAST;

// ── Compiler Directive nodes (@) ──────────────────────────────────────────────
// Defined in DeclAST.hpp and ExprAST.hpp respectively.
struct AttributeAST;         // @name or @name(args) — attached to declarations
struct AttributeArgAST;      // argument inside an attribute's parentheses
struct IntrinsicCallExprAST; // @name(args) — in expression position

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
    InternedString  text;   // Markdown content, ' -' prefix already stripped
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
    int             line   = 0;
    int             column = 0;
    InternedString  file;

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
//
// Reference: docs/LUC_SEMANTIC.md for more details
// ─────────────────────────────────────────────────────────────────────────────

struct ASTVisitor {

    virtual ~ASTVisitor() = default;

    // ── Type nodes ────────────────────────────────────────────────────────────
    virtual void visit(PrimitiveTypeAST&)      {}
    virtual void visit(NamedTypeAST&)          {}
    virtual void visit(NullableTypeAST&)       {}
    virtual void visit(FixedArrayTypeAST&)     {}   // [N]T
    virtual void visit(SliceTypeAST&)          {}   // []T
    virtual void visit(DynamicArrayTypeAST&)   {}   // [*]T
    virtual void visit(RefTypeAST&)            {}
    virtual void visit(PtrTypeAST&)            {}
    virtual void visit(FuncTypeAST&)           {}

    // ── Declaration nodes ─────────────────────────────────────────────────────
    virtual void visit(PackageDeclAST&)     {}
    virtual void visit(UseDeclAST&)         {}
    virtual void visit(VarDeclAST&)         {}
    virtual void visit(FuncDeclAST&)        {}
    virtual void visit(StructDeclAST&)      {}
    virtual void visit(FieldDeclAST&)       {}
    virtual void visit(EnumDeclAST&)        {}
    virtual void visit(EnumVariantAST&)     {}
    virtual void visit(TraitMethodAST&)     {}
    virtual void visit(TraitDeclAST&)       {}
    virtual void visit(TraitRefAST&)        {}
    virtual void visit(ImplDeclAST&)        {}
    virtual void visit(MethodDeclAST&)      {}
    virtual void visit(FromDeclAST&)        {}
    virtual void visit(FromEntryAST&)       {}
    virtual void visit(TypeAliasDeclAST&)   {}
    virtual void visit(ParamAST&)           {}
    virtual void visit(GenericParamAST&)    {}
    

    // ── Expression nodes ──────────────────────────────────────────────────────
    virtual void visit(LiteralExprAST&)         {}
    virtual void visit(IdentifierExprAST&)      {}
    virtual void visit(ArrayLiteralExprAST&)    {}
    virtual void visit(StructLiteralExprAST&)   {}
    virtual void visit(FieldInitAST&)           {}
    virtual void visit(BinaryExprAST&)          {}
    virtual void visit(UnaryExprAST&)           {}
    virtual void visit(CallExprAST&)            {}
    virtual void visit(IndexExprAST&)           {}
    virtual void visit(FieldAccessExprAST&)     {}
    virtual void visit(BehaviorAccessExprAST&)  {}
    virtual void visit(NullableChainExprAST&)   {}
    virtual void visit(NullCoalesceExprAST&)    {}
    virtual void visit(AssignExprAST&)          {}
    virtual void visit(IsExprAST&)              {}
    virtual void visit(PipelineExprAST&)        {}
    virtual void visit(PipelineStepAST&)        {}
    virtual void visit(ComposeExprAST&)         {}
    virtual void visit(ComposeOperandAST&)      {}
    virtual void visit(AnonFuncExprAST&)        {}
    virtual void visit(AwaitExprAST&)           {}
    virtual void visit(MatchExprAST&)           {}
    virtual void visit(IfExprAST&)              {} 
    virtual void visit(RangeExprAST&)           {}
    virtual void visit(TypeConvExprAST&)        {}

    // ── Pattern nodes ─────────────────────────────────────────────────────────
    virtual void visit(BindPatternAST&)         {}
    virtual void visit(WildcardPatternAST&)     {}
    virtual void visit(TypePatternAST&)         {}
    virtual void visit(StructPatternAST&)       {}
    virtual void visit(FieldPatternAST&)        {}
    virtual void visit(PatternExprAST&)         {}
    virtual void visit(MatchArmAST&)            {}
    virtual void visit(DefaultArmAST&)          {}

    // ── Statement nodes ───────────────────────────────────────────────────────
    virtual void visit(BlockStmtAST&)           {}
    virtual void visit(ExprStmtAST&)            {}
    virtual void visit(DeclStmtAST&)            {}
    virtual void visit(IfStmtAST&)              {}
    virtual void visit(SwitchStmtAST&)          {}
    virtual void visit(SwitchCaseAST&)          {}
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

    // ── Compiler Directive nodes (@) ──────────────────────────────────────────
    virtual void visit(AttributeAST&)           {}
    virtual void visit(AttributeArgAST&)        {}
    virtual void visit(IntrinsicCallExprAST&)   {}

    // ── Unknown / Recovery nodes ──────────────────────────────────────────────
    virtual void visit(UnknownDeclAST&)         {}
    virtual void visit(UnknownExprAST&)         {}
    virtual void visit(UnknownStmtAST&)         {}
    virtual void visit(UnknownTypeAST&)         {}
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

// ─────────────────────────────────────────────────────────────────────────────
// Effect — Behavioural bitmask
// ─────────────────────────────────────────────────────────────────────────────

enum class Effect : uint32_t {
    None          = 0,
    SideEffect    = 1 << 0,  // writes memory, calls impure function, I/O
    IsAsync       = 1 << 1,  // contains await or async call
    WritesMemory  = 1 << 2,  // direct assignment to memory location
    ReadsMemory   = 1 << 3,  // dereference or non-const field access
    Tainted       = 1 << 4,  // raw pointer (*T) involved, FFI boundary
};

inline constexpr Effect operator|(Effect a, Effect b) { return static_cast<Effect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
inline bool hasEffect(uint32_t flags, Effect e) { return (flags & static_cast<uint32_t>(e)) != 0; }

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
    bool    isConst          = false;    // true → compile-time constant (const decl or literal)
    int     scopeDepth       = 0;        // 0 = file scope, +1 per nested block

    // ── Structural ────────────────────────────────────────────────────────────
    BaseAST* parent          = nullptr;  // set by parser when child is attached

    // ── Behavioural bitmask ───────────────────────────────────────────────────
    uint32_t effectFlags     = 0;

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

using TypePtr    = std::unique_ptr<TypeAST, ASTDeleter>;
using DeclPtr    = std::unique_ptr<DeclAST, ASTDeleter>;
using ExprPtr    = std::unique_ptr<ExprAST, ASTDeleter>;
using StmtPtr    = std::unique_ptr<StmtAST, ASTDeleter>;
using PatternPtr = std::unique_ptr<PatternAST, ASTDeleter>;

// ─────────────────────────────────────────────────────────────────────────────
// ProgramAST — root of the entire tree
//
// The parser produces exactly one ProgramAST per source file.
// The semantic pass walks all files' ProgramASTs to resolve imports.
// ─────────────────────────────────────────────────────────────────────────────

struct ProgramAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    InternedString         packageName;   // from `package foo`
    InternedString         filePath;      // relative path, e.g. "math/vec2.luc"
    std::vector<DeclPtr>   decls;         // top-level declarations in order

    ProgramAST() : BaseAST(ASTKind::Program) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// UnknownAST definition and helpers
// ─────────────────────────────────────────────────────────────────────────────

struct UnknownAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Unknown;
    
    UnknownAST() : BaseAST(ASTKind::Unknown) {}
    
    void accept(ASTVisitor& v) override { 
        // No-op or log warning
        LUC_LOG_SEMANTIC("visit(UnknownAST) - this should not happen");
    }
};

// Singleton instance for null replacement
inline UnknownAST* unknownAST() {
    static UnknownAST instance;
    return &instance;
}

// Helper to check if node is unknown/null-equivalent
inline bool isUnknown(const BaseAST* node) {
    if (!node) return true;
    switch (node->kind) {
        case ASTKind::Unknown:
        case ASTKind::UnknownDecl:
        case ASTKind::UnknownExpr:
        case ASTKind::UnknownStmt:
        case ASTKind::UnknownType:
            return true;
        default:
            return false;
    }
}

struct UnknownDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownDecl;
    UnknownDeclAST() : DeclAST(ASTKind::UnknownDecl) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

struct UnknownExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownExpr;
    UnknownExprAST() : ExprAST(ASTKind::UnknownExpr) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

struct UnknownStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownStmt;
    UnknownStmtAST() : StmtAST(ASTKind::UnknownStmt) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

struct UnknownTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownType;
    UnknownTypeAST() : TypeAST(ASTKind::UnknownType) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// Helper factory functions
inline DeclPtr makeUnknownDecl(SourceLocation loc = {}) {
    auto node = std::make_unique<UnknownDeclAST>();
    node->loc = loc;
    return DeclPtr(node.release(), ASTDeleter{});
}

inline ExprPtr makeUnknownExpr(SourceLocation loc = {}) {
    auto node = std::make_unique<UnknownExprAST>();
    node->loc = loc;
    return ExprPtr(node.release(), ASTDeleter{});
}

inline StmtPtr makeUnknownStmt(SourceLocation loc = {}) {
    auto node = std::make_unique<UnknownStmtAST>();
    node->loc = loc;
    return StmtPtr(node.release(), ASTDeleter{});
}

inline TypePtr makeUnknownType(SourceLocation loc = {}) {
    auto node = std::make_unique<UnknownTypeAST>();
    node->loc = loc;
    return TypePtr(node.release(), ASTDeleter{});
}