/**
 * @file BaseAST.hpp
 *
 * @responsibility The Foundation. Defines the BaseAST, the Visitor interface,
 *                 and common types (DocComment, SourceLocation, ASTKind).
 *
 * @architectural_note
 *   This file uses Forward Declarations for all AST families (Expr, Stmt, etc.).
 *   NEVER include a family header (like ExprAST.hpp) here; this keeps the
 *   dependency graph acyclic.
 *
 * @related_files
 *   - src/ast/ExprAST.hpp, StmtAST.hpp, DeclAST.hpp, TypeAST.hpp
 *   - Each family header includes BaseAST.hpp, not the other way around.
 */

#pragma once

#include "debug/DebugMacros.hpp"
#include "support/ASTArena.hpp"
#include "support/InternedString.hpp"
#include "support/ArenaSpan.hpp"

#include <string>
#include <optional>
#include <memory>
#include <vector>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations — every AST family forward-declared here so any header
// can accept a visitor or hold a pointer without pulling in the full family.
//
// The actual struct definitions live in their own headers:
//   TypeAST.hpp     — PrimitiveTypeAST, NamedTypeAST, FixedArrayTypeAST, ...
//   DeclAST.hpp     — FuncDeclAST, StructDeclAST, ImplDeclAST, ...
//   ExprAST.hpp     — LiteralExprAST, CallExprAST, PipelineExprAST, ...
//   StmtAST.hpp     — BlockStmtAST, ForStmtAST, ...
// ─────────────────────────────────────────────────────────────────────────────

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct NullableTypeAST;
struct FixedArrayTypeAST;
struct SliceTypeAST;
struct DynamicArrayTypeAST;
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
struct EnumVariantAST;
struct EnumDeclAST;
struct TraitMethodAST;
struct TraitDeclAST;
struct TraitRefAST;
struct MethodDeclAST;
struct FromDeclAST;
struct FromEntryAST;
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
struct StaticAccessExprAST;
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

// Pattern nodes (defined in ExprAST.hpp)
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
struct MultiVarDeclAST;
struct MultiAssignStmtAST;

// Root
struct ProgramAST;

// Unknown nodes
struct UnknownDeclAST;
struct UnknownExprAST;
struct UnknownStmtAST;
struct UnknownTypeAST;

// Compiler Directive nodes
struct AttributeAST;
struct AttributeArgAST;
struct IntrinsicCallExprAST;

// ─────────────────────────────────────────────────────────────────────────────
// ASTKind — compile-time tag stored on every node.
//
// Replaces runtime RTTI / dynamic_cast with a single integer comparison.
// Every concrete node defines `static constexpr ASTKind staticKind` and passes
// it to the BaseAST constructor.
//
// Usage:
//   if (node->kind == ASTKind::PrimitiveType) {
//       auto* p = static_cast<PrimitiveTypeAST*>(node);
//   }
//
// Or use the helpers on BaseAST:
//   if (node->isa<PrimitiveTypeAST>()) { node->as<PrimitiveTypeAST>() ... }
// ─────────────────────────────────────────────────────────────────────────────

enum class ASTKind : uint16_t {
    Unknown,
    UnknownDecl,
    UnknownExpr,
    UnknownStmt,
    UnknownType,

    // Type nodes
    PrimitiveType,
    NamedType,
    NullableType,
    FixedArrayType,
    SliceType,
    DynamicArrayType,
    RefType,
    PtrType,
    FuncType,

    // Declaration nodes
    PackageDecl,
    UseDecl,
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

    // Expression nodes
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
    IfExpr,
    RangeExpr,
    TypeConvExpr,

    // Statement nodes
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
    MultiVarDecl,
    MultiAssignStmt,

    // Pattern nodes
    BindPattern,
    WildcardPattern,
    TypePattern,
    StructPattern,
    FieldPattern,
    PatternExpr,
    MatchArm,
    DefaultArm,

    // Root
    Program,

    // Compiler directives
    Attribute,
    AttributeArg,
    IntrinsicCallExpr,
};

// ─────────────────────────────────────────────────────────────────────────────
// IndexKind — distinguishes array element access vs slice access.
// ─────────────────────────────────────────────────────────────────────────────

enum class IndexKind {
    Element,  // expr '[' expr ']'              — nums[2]
    Slice,    // expr '[' expr '..' expr ']'    — nums[1..3] (inclusive)
};

// ─────────────────────────────────────────────────────────────────────────────
// DocComment — documentation attached to declarations only (stored in DeclAST).
// ─────────────────────────────────────────────────────────────────────────────

enum class DocCommentForm {
    Stacked,   // consecutive '--' lines above declaration
    Block,     // /-- ... --/ block above declaration
    Trailing,  // '--' comment on same line as declaration
};

struct DocComment {
    InternedString  text;   // Markdown content, with ' -' prefix already stripped
    DocCommentForm  form;
};

// ─────────────────────────────────────────────────────────────────────────────
// SourceLocation — packed into 32 bits (20 bits line, 12 bits column).
// File path is stored once in ProgramAST, not per node.
// ─────────────────────────────────────────────────────────────────────────────

struct SourceLocation {
    uint32_t value = 0;  // line in high 20 bits, column in low 12 bits

    SourceLocation() = default;
    SourceLocation(uint32_t line, uint32_t column) {
        value = (line << 12) | (column & 0xFFF);
    }

    uint32_t line()   const { return value >> 12; }
    uint32_t column() const { return value & 0xFFF; }
    bool isKnown()    const { return value > 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// ASTVisitor — abstract base for all AST passes.
// Each concrete node's `accept()` calls the corresponding `visit()` method.
// Default implementations do nothing, so passes only override needed methods.
// ─────────────────────────────────────────────────────────────────────────────

struct ASTVisitor {
    virtual ~ASTVisitor() = default;

    // Type nodes
    virtual void visit(PrimitiveTypeAST&)      {}
    virtual void visit(NamedTypeAST&)          {}
    virtual void visit(NullableTypeAST&)       {}
    virtual void visit(FixedArrayTypeAST&)     {}
    virtual void visit(SliceTypeAST&)          {}
    virtual void visit(DynamicArrayTypeAST&)   {}
    virtual void visit(RefTypeAST&)            {}
    virtual void visit(PtrTypeAST&)            {}
    virtual void visit(FuncTypeAST&)           {}

    // Declaration nodes
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

    // Expression nodes
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
    virtual void visit(StaticAccessExprAST&)    {}
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

    // Pattern nodes
    virtual void visit(BindPatternAST&)         {}
    virtual void visit(WildcardPatternAST&)     {}
    virtual void visit(TypePatternAST&)         {}
    virtual void visit(StructPatternAST&)       {}
    virtual void visit(FieldPatternAST&)        {}
    virtual void visit(PatternExprAST&)         {}
    virtual void visit(MatchArmAST&)            {}
    virtual void visit(DefaultArmAST&)          {}

    // Statement nodes
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
    virtual void visit(MultiVarDeclAST&)        {}
    virtual void visit(MultiAssignStmtAST&)     {}

    // Root
    virtual void visit(ProgramAST&)             {}

    // Compiler directives
    virtual void visit(AttributeAST&)           {}
    virtual void visit(AttributeArgAST&)        {}
    virtual void visit(IntrinsicCallExprAST&)   {}

    // Unknown / recovery nodes
    virtual void visit(UnknownDeclAST&)         {}
    virtual void visit(UnknownExprAST&)         {}
    virtual void visit(UnknownStmtAST&)         {}
    virtual void visit(UnknownTypeAST&)         {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Effect — behavioural bitmask for semantic analysis.
// ─────────────────────────────────────────────────────────────────────────────

enum class Effect : uint32_t {
    None          = 0,
    SideEffect    = 1 << 0,  // writes memory, calls impure function, I/O
    IsAsync       = 1 << 1,  // contains await or async call
    WritesMemory  = 1 << 2,  // direct assignment to memory location
    ReadsMemory   = 1 << 3,  // dereference or non-const field access
    Tainted       = 1 << 4,  // raw pointer (*T) involved, FFI boundary
};

inline constexpr Effect operator|(Effect a, Effect b) {
    return static_cast<Effect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool hasEffect(uint32_t flags, Effect e) {
    return (flags & static_cast<uint32_t>(e)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// BaseAST — root of the entire AST hierarchy.
//
// This struct is deliberately small. Node‑specific data (doc comments,
// attributes, resolved types) is pushed down to the family bases (DeclAST,
// ExprAST, etc.) to minimise per‑node memory footprint.
//
// Fields:
//   kind            — discriminator for LLVM‑style RTTI (isa/as)
//   loc             — source location (packed line+column)
//   isBehaviorMember — set by semantic pass for Type:method references
//   isConst         — compile‑time constant value
//   scopeDepth      — nesting depth (0 = file scope)
//   effectFlags     — bitmask of Effect values
//
// No `doc`, `attributes`, or `resolvedType` here – they belong to DeclAST
// and ExprAST/PatternAST respectively.
// ─────────────────────────────────────────────────────────────────────────────

struct BaseAST {
    ASTKind kind;
    SourceLocation loc;

    // Semantic annotations (filled by semantic pass)
    bool    isBehaviorMember = false;
    bool    isConst          = false;
    int     scopeDepth       = 0;
    uint32_t effectFlags     = 0;

    explicit BaseAST(ASTKind k) : kind(k) {}
    virtual ~BaseAST() = default;

    // Visitor dispatch – every concrete node must implement this.
    virtual void accept(ASTVisitor& visitor) = 0;

    // Kind‑based type checking (no RTTI overhead)
    template<typename T>
    bool isa() const { return kind == T::staticKind; }

    template<typename T>
    T* as() {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>()");
        return static_cast<T*>(this);
    }

    template<typename T>
    const T* as() const {
        assert(kind == T::staticKind && "ASTKind mismatch in as<T>()");
        return static_cast<const T*>(this);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeArgKind — discriminator for attribute argument literals.
// ─────────────────────────────────────────────────────────────────────────────

enum class AttributeArgKind {
    StringLit,   // "string"
    IntLit,      // 42, 0xFF, 0b1010
    BoolLit,     // true, false
    TypeIdent    // TypeName (e.g., @extern("malloc", C))
};

struct AttributeArgAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::AttributeArg;

    AttributeArgKind kind;
    InternedString   value;   // raw source text

    AttributeArgAST(AttributeArgKind k, InternedString v)
        : BaseAST(ASTKind::AttributeArg), kind(k), value(v) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using AttributeArgPtr = ASTPtr<AttributeArgAST>;

struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    InternedString name;
    ArenaSpan<AttributeArgPtr> args;   // arguments, if any

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using AttributePtr = ASTPtr<AttributeAST>;

// ─────────────────────────────────────────────────────────────────────────────
// Family bases – these add minimal data to enable heterogeneous collections.
// They forward the kind tag to BaseAST.
// ─────────────────────────────────────────────────────────────────────────────

struct TypeAST    : BaseAST { explicit TypeAST(ASTKind k)    : BaseAST(k) {} };

struct DeclAST    : BaseAST {
    std::optional<DocComment> doc;           // documentation comment
    ArenaSpan<AttributePtr>   attributes;    // compiler directives

    explicit DeclAST(ASTKind k) : BaseAST(k) {}
    bool hasDoc() const { return doc.has_value(); }
};

struct ExprAST    : BaseAST {
    TypeAST* resolvedType = nullptr;   // set by semantic pass

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};

struct StmtAST    : BaseAST { explicit StmtAST(ASTKind k) : BaseAST(k) {} };

struct PatternAST : BaseAST {
    TypeAST* resolvedType = nullptr;   // set by semantic pass

    explicit PatternAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Ownership aliases – each pointer uses ASTDeleter (no‑op) because nodes are
// arena‑allocated and freed in bulk.
// ─────────────────────────────────────────────────────────────────────────────

using TypePtr    = std::unique_ptr<TypeAST, ASTDeleter>;
using DeclPtr    = std::unique_ptr<DeclAST, ASTDeleter>;
using ExprPtr    = std::unique_ptr<ExprAST, ASTDeleter>;
using StmtPtr    = std::unique_ptr<StmtAST, ASTDeleter>;
using PatternPtr = std::unique_ptr<PatternAST, ASTDeleter>;

// ─────────────────────────────────────────────────────────────────────────────
// ProgramAST – root node for a single translation unit (source file).
// ─────────────────────────────────────────────────────────────────────────────

struct ProgramAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    InternedString     packageName;   // from `package foo`
    InternedString     filePath;      // relative path (e.g., "math/vec2.luc")
    ArenaSpan<DeclPtr> decls;         // top‑level declarations in source order

    ProgramAST() : BaseAST(ASTKind::Program) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamAST – a generic type parameter (e.g., `<T : Drawable>`).
// ─────────────────────────────────────────────────────────────────────────────

struct GenericParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParam;

    InternedString name;
    ArenaSpan<InternedString> constraints;   // trait names (empty = unconstrained)

    explicit GenericParamAST(InternedString n)
        : BaseAST(ASTKind::GenericParam), name(n) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ParamAST – a function parameter (name, type, variadic flag).
// ─────────────────────────────────────────────────────────────────────────────

struct ParamAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Param;

    InternedString name;
    TypePtr        type;
    bool           isVariadic = false;

    ParamAST() : BaseAST(ASTKind::Param) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using ParamPtr      = ASTPtr<ParamAST>;
using ParamGroup    = ArenaSpan<ParamPtr>;
using GenericParamPtr = ASTPtr<GenericParamAST>;

// ─────────────────────────────────────────────────────────────────────────────
// UnknownAST family – error recovery nodes, never null.
// ─────────────────────────────────────────────────────────────────────────────

struct UnknownAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Unknown;

    UnknownAST() : BaseAST(ASTKind::Unknown) {}

    void accept(ASTVisitor& v) override {
        LUC_LOG_SEMANTIC("visit(UnknownAST) – this should not happen");
    }
};

inline UnknownAST* unknownAST() {
    static UnknownAST instance;
    return &instance;
}

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