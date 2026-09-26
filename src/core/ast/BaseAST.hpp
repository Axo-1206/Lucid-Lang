/**
 * @file BaseAST.hpp
 *
 * @responsibility The foundation of the AST. Defines BaseAST, the
 *                 visitor-free `isa`/`as` helpers, the ASTKind enum, and
 *                 the common types every AST node shares.
 *
 * ─── Design: the AST is the compiler's only tree ──────────────────────────
 * Every stage of the compiler — parser, Sema, bytecode compiler, and any
 * future backend — reads the same AST. There is no IR between the AST
 * and bytecode; the bytecode compiler walks these nodes and emits
 * instructions. The AST is what the frontend produces and what every
 * backend consumes.
 *
 * ─── Design: three declaration forms, and only three ──────────────────────
 * A module contains TABLE declarations, FN declarations, and variable
 * declarations. There is no `struct`, no `enum`, no `trait`, no `DEF`,
 * no `satisfy`. Every name a program introduces comes from one of the
 * three forms.
 *
 * ─── Design: one function representation ──────────────────────────────────
 * There is no `fn`/`cls` distinction, no closures, no captures. Every
 * function value is a compile-time-known code pointer. A function
 * literal — a lambda — is a single-expression function whose body can
 * only reference its own parameters and module-level declarations; it is
 * lowered to an ordinary top-level function by the compiler.
 *
 * ─── Design: no nullable or fallible type suffixes ────────────────────────
 * There is no `T?` or `T!`. A row reference (`&T`) is inherently nilable;
 * `nil` is an ordinary value of that type. Primitives and bare tables
 * are never nilable. The `??` operator coalesces `nil` to a fallback.
 *
 * ─── Design: attributes are juxtaposed ────────────────────────────────────
 * An attribute is `@` followed by an identifier, optionally with a
 * parenthesized argument list. Attributes are juxtaposed without
 * brackets or commas: `@export @on(EventKind.KeyDown)`. The parser reads
 * them as a sequence of `AttributeAST` nodes attached to the declaration
 * that follows.
 *
 * ─── Design: the AST holds no codegen facts ───────────────────────────────
 * No `llvmType`, no `llvmFunction`, no `bytecodeOffset`. Every
 * codegen-side fact lives on a codegen-side object keyed by AST pointer.
 * The reason: the AST outlives any single codegen run (hot reload lowers
 * the same AST against new targets), so a cached codegen fact on the AST
 * would be a snapshot of the wrong run.
 *
 * ─── Design: family forward declarations keep the graph acyclic ───────────
 * Every concrete AST node is forward-declared here. Family headers
 * (`DeclAST.hpp`, `ExprAST.hpp`, `StmtAST.hpp`, `TypeAST.hpp`) include
 * this file and define their concrete nodes. This file never includes a
 * family header, so the dependency graph is acyclic.
 */

#pragma once

#include "../SourceLocation.hpp"
#include "../memory/ASTArena.hpp"
#include "../memory/InternedString.hpp"
#include "../memory/ArenaSpan.hpp"
#include "../diagnostics/StackTrace.hpp"
#include "ResourceKind.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Family forward declarations
// ─────────────────────────────────────────────────────────────────────────────

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct ArrayTypeAST;
struct RowRefTypeAST;
struct FunctionTypeAST;

// DeclAST.hpp
struct ImportDeclAST;
struct TableDeclAST;
struct ColumnDeclAST;
struct FnDeclAST;
struct VarDeclAST;
struct ParamAST;

// ExprAST.hpp
struct LiteralExprAST;
struct IdentifierExprAST;
struct ArrayLiteralExprAST;
struct FieldAccessExprAST;
struct ModuleAccessExprAST;
struct IndexExprAST;
struct CallExprAST;
struct LambdaExprAST;
struct StartExprAST;
struct UnaryExprAST;
struct BinaryExprAST;
struct ParenExprAST;
struct RangeExprAST;

// StmtAST.hpp
struct BlockStmtAST;
struct VarDeclStmtAST;
struct AssignStmtAST;
struct ExprStmtAST;
struct ReturnStmtAST;
struct BreakStmtAST;
struct ContinueStmtAST;
struct IfStmtAST;
struct SwitchStmtAST;
struct SwitchCaseAST;
struct WhileStmtAST;
struct ForStmtAST;

// Sequence suspend points (§9.2)
struct WaitStmtAST;
struct WaitFramesStmtAST;
struct WaitUntilStmtAST;
struct WaitForEventStmtAST;
struct WaitForRequestStmtAST;

// Root and shared
struct ModuleAST;
struct AttributeAST;

// Special family bases
struct ValueDeclAST;
struct TypeDeclAST;

// Error-recovery nodes
struct UnknownDeclAST;
struct UnknownExprAST;
struct UnknownStmtAST;
struct UnknownTypeAST;

// ─────────────────────────────────────────────────────────────────────────────
// ASTKind
// ─────────────────────────────────────────────────────────────────────────────
//
// Compile-time tag on every node. Replaces RTTI with one integer
// comparison. Every concrete node declares `static constexpr ASTKind
// staticKind` and passes it to its family base's constructor.
//
// The enum is ordered so nodes of the same family are contiguous. The
// family bases are first, then each family's concrete nodes, then the
// root and shared nodes. Ordering is an implementation detail; the
// `isa`/`as` helpers and the family base types are the contract.

enum class ASTKind : uint16_t {
    // ─── Family bases ───────────────────────────────────────────────────
    ValueDecl,
    TypeDecl,

    // ─── Type nodes ─────────────────────────────────────────────────────
    PrimitiveType,
    NamedType,
    ArrayType,
    RowRefType,
    FunctionType,

    // ─── Declaration nodes ──────────────────────────────────────────────
    ImportDecl,
    TableDecl,
    ColumnDecl,
    FnDecl,
    VarDecl,
    Param,

    // ─── Expression nodes ───────────────────────────────────────────────
    LiteralExpr,
    IdentifierExpr,
    ArrayLiteralExpr,
    FieldAccessExpr,
    ModuleAccessExpr,
    IndexExpr,
    CallExpr,
    LambdaExpr,
    StartExpr,
    UnaryExpr,
    BinaryExpr,
    AssignExpr,
    ParenExpr,
    RangeExpr,

    // ─── Statement nodes ────────────────────────────────────────────────
    BlockStmt,
    VarDeclStmt,
    AssignStmt,
    ExprStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    IfStmt,
    SwitchStmt,
    SwitchCase,
    WhileStmt,
    ForStmt,

    // ─── Sequence suspend points ────────────────────────────────────────
    WaitStmt,
    WaitFramesStmt,
    WaitUntilStmt,
    WaitForEventStmt,
    WaitForRequestStmt,

    // ─── Root and shared ────────────────────────────────────────────────
    Module,
    Attribute,

    // ─── Error recovery ─────────────────────────────────────────────────
    Unknown,
    UnknownDecl,
    UnknownExpr,
    UnknownStmt,
    UnknownType,
};

// ─────────────────────────────────────────────────────────────────────────────
// DocComment
// ─────────────────────────────────────────────────────────────────────────────
//
// A doc comment is attached to the declaration that follows it. The
// grammar's doc-comment form is `/-- ... --/`; the parser's harvester
// reads it and attaches the text to the declaration's `doc` field.

struct DocComment {
    /// The comment's text, with the leading and trailing delimiters
    /// stripped. Interior whitespace and newlines are preserved.
    InternedString text;
};

// ─────────────────────────────────────────────────────────────────────────────
// AST_ASSERT_MSG
// ─────────────────────────────────────────────────────────────────────────────
//
// Invariant check for compiler bugs. Unlike `assert`, this:
//   - always fires (not disabled by NDEBUG),
//   - prints file, line, function, and a caller-supplied message,
//   - prints a stack trace before aborting.
//
// Use ONLY for conditions that indicate a bug in the compiler itself.
// User-facing diagnostics go through DiagnosticEngine.
//
// The message is a string literal — no allocation, no formatting.

#define AST_ASSERT_MSG(cond, msg)                                              \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr,                                               \
                "\nAssertion failed: %s\n"                                     \
                "  File:     %s\n"                                             \
                "  Line:     %d\n"                                             \
                "  Function: %s\n"                                             \
                "  Message:  %s\n",                                            \
                #cond, __FILE__, __LINE__, __func__, (msg));                   \
            ::lucid::diag::printStackTrace(/*skipFrames=*/2);                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// BaseAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The root of the AST hierarchy.
///
/// Every node carries its `ASTKind` tag, its source location, and a
/// `hasSyntaxError` flag. The family bases (`DeclAST`, `ExprAST`,
/// `StmtAST`, `TypeAST`) add category-specific fields; the concrete
/// nodes add their own.
struct BaseAST {
    /// The compile-time tag. Set once by the concrete node's constructor
    /// and never changed.
    ASTKind kind;

    /// The node's source location. The convention is: a node's location
    /// is the location of the *first* token of the construct it
    /// represents. A compound node's location is the location of its
    /// leftmost or leading token.
    SourceLocation loc;

    /// True if the node was produced by a parser error-recovery path. A
    /// node with this flag set is structurally valid but represents
    /// source that had a syntax error; Sema skips it or treats its
    /// missing fields as unknowns. The flag propagates upward: a node
    /// whose subtree contains a marked node is itself marked.
    bool hasSyntaxError = false;

    explicit BaseAST(ASTKind k) : kind(k) {}
    virtual ~BaseAST() = default;

    // ─── Type checking ──────────────────────────────────────────────────

    template <typename T>
    bool isa() const { return kind == T::staticKind; }

    template <typename T>
    T* as() {
        AST_ASSERT_MSG(kind == T::staticKind,
                       "ASTKind mismatch in as<T>() — caller assumed the "
                       "wrong node type");
        return static_cast<T*>(this);
    }

    template <typename T>
    const T* as() const {
        AST_ASSERT_MSG(kind == T::staticKind,
                       "ASTKind mismatch in as<T>() — caller assumed the "
                       "wrong node type");
        return static_cast<const T*>(this);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Family bases
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base for all declaration nodes.
///
/// Every declaration has:
///   - a name (interned string),
///   - an attribute list,
///   - an optional doc comment,
///   - a `declaringModule` pointer (set by Sema during module registration),
///   - an `isExported` flag (set by Sema when it sees the `@export` attribute).
///
/// The three concrete declaration forms are `TableDeclAST`, `FnDeclAST`,
/// and `VarDeclAST` (and `ImportDeclAST`, which is a directive rather
/// than a name-binding declaration).
struct DeclAST : BaseAST {
    InternedString name;
    ArenaSpan<AttributeAST*> attributes;
    std::optional<DocComment> doc;

    /// Set by Sema during module registration. Used for mangled-name
    /// computation and diagnostics that need the declaring module.
    ModuleAST* declaringModule = nullptr;

    /// True if the declaration is visible outside its module. Set by Sema
    /// when the declaration's attribute list contains `@export`.
    bool isExported = false;

    explicit DeclAST(ASTKind k, InternedString n)
        : BaseAST(k), name(n) {}

    bool hasDoc() const { return doc.has_value(); }
};

/// @brief Base for all type-annotation nodes.
struct TypeAST : BaseAST {
    explicit TypeAST(ASTKind k) : BaseAST(k) {}
};

/// @brief Base for all statement nodes.
struct StmtAST : BaseAST {
    explicit StmtAST(ASTKind k) : BaseAST(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONSTANT EVALUATION
// ─────────────────────────────────────────────────────────────────────────────
//
// The grammar evaluates constant expressions during compilation. In the
// redesigned language, the primary use is fixed-table rows (§4.1.1a):
// every inline row is resolvable entirely at compile time, and the
// compiler bakes the result into the compiled artifact.
//
// The same metadata-on-the-original-node approach is used: the constant
// value is stored alongside the expression, not by replacing the
// expression subtree. See the field docs on ExprAST.

/// @brief The result of compile-time constant evaluation.
///
/// A default-constructed `ConstantValue` has `Kind::Unknown`, which
/// `isEvaluated()` reports as false. The invariant `isConst ==
/// constValue.isEvaluated()` is maintained by the constant evaluator.
///
/// The set of kinds is smaller than the previous design's because the
/// grammar has fewer expression forms. `Struct` and `Enum` are gone
/// (tables replace both), and `Array` remains for fixed-size array
/// constants. `Function` remains because a fixed table can hold a
/// function-typed column (§6.9) initialized from a top-level `FN` name.
struct ConstantValue {
    enum class Kind : uint8_t {
        Unknown,    ///< Not yet evaluated
        Error,      ///< Evaluation failed
        Void,       ///< No value (a `unit`-returning construct)
        Bool,       ///< true / false
        Int,        ///< Integer (any width)
        Float,      ///< Floating point (any precision)
        String,     ///< String literal
        Char,       ///< Character literal
        Nil,        ///< the `nil` value
        Array,      ///< A fixed-size array constant
        Function,   ///< A top-level `FN` name used as a value
    };

    Kind      kind = Kind::Unknown;
    TypeAST*  type = nullptr;

    /// @brief Value storage.
    ///
    /// The `FuncDeclAST*` case requires `FuncDeclAST` to be a complete
    /// type at the point the variant is instantiated. Any translation
    /// unit that stores a function value in a `ConstantValue` must
    /// include `DeclAST.hpp` first. This is the existing convention.
    std::variant<
        std::monostate,                  // Unknown, Error, Void, Nil
        bool,                            // Bool
        int64_t,                         // Int
        double,                          // Float
        InternedString,                  // String, Char
        std::vector<ConstantValue>,      // Array
        FnDeclAST*                       // Function
    > value;

    ConstantValue() : kind(Kind::Unknown) {}

    explicit ConstantValue(bool v)           : kind(Kind::Bool),   value(v) {}
    explicit ConstantValue(int64_t v)        : kind(Kind::Int),    value(v) {}
    explicit ConstantValue(double v)         : kind(Kind::Float),  value(v) {}
    explicit ConstantValue(InternedString v) : kind(Kind::String), value(v) {}
    explicit ConstantValue(FnDeclAST* f)     : kind(Kind::Function), value(f) {}

    // ─── Factories ──────────────────────────────────────────────────────

    static ConstantValue unknown()   { return ConstantValue(); }
    static ConstantValue error()     { ConstantValue v; v.kind = Kind::Error; return v; }
    static ConstantValue voidValue() { ConstantValue v; v.kind = Kind::Void;  return v; }
    static ConstantValue nil()       { ConstantValue v; v.kind = Kind::Nil;   return v; }

    // ─── Predicates ─────────────────────────────────────────────────────

    bool isEvaluated() const { return kind != Kind::Unknown && kind != Kind::Error; }
    bool isError()     const { return kind == Kind::Error; }
    bool isUnknown()   const { return kind == Kind::Unknown; }
    bool isVoid()      const { return kind == Kind::Void; }
    bool isBool()      const { return kind == Kind::Bool; }
    bool isInt()       const { return kind == Kind::Int; }
    bool isFloat()     const { return kind == Kind::Float; }
    bool isString()    const { return kind == Kind::String; }
    bool isChar()      const { return kind == Kind::Char; }
    bool isNil()       const { return kind == Kind::Nil; }
    bool isArray()     const { return kind == Kind::Array; }
    bool isFunction()  const { return kind == Kind::Function; }

    // ─── Accessors ──────────────────────────────────────────────────────

    bool           asBool()     const { return std::get<bool>(value); }
    int64_t        asInt()      const { return std::get<int64_t>(value); }
    double         asFloat()    const { return std::get<double>(value); }
    InternedString asString()   const { return std::get<InternedString>(value); }
    FnDeclAST*     asFunction() const { return std::get<FnDeclAST*>(value); }

    const std::vector<ConstantValue>& asArray() const {
        return std::get<std::vector<ConstantValue>>(value);
    }
    std::vector<ConstantValue>& asArrayMut() {
        return std::get<std::vector<ConstantValue>>(value);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ExprAST — base for all expressions
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base for all expression nodes.
///
/// Every expression carries the semantic fields Sema writes and later
/// passes read. The expression's *resolved type* is the compiler's
/// representation of what the expression produces; the `isConst` and
/// `constValue` fields carry the constant-evaluation result when the
/// expression is a compile-time constant.
///
/// There is no `valueState` field in the redesigned grammar. The old
/// design's `ValueState` tracked a value's nil/err/definite state for
/// flow-sensitive narrowing. Under the new grammar, only row references
/// are nilable, and narrowing is a check against `nil` (`x == nil`,
/// `x != nil`), which the type checker can handle through ordinary
/// flow analysis without a dedicated `ValueState` per expression. If a
/// future feature needs richer flow-state tracking, a field can be
/// added; today it would be dead weight.
struct ExprAST : BaseAST {
    /// The resolved type of this expression. Set by Sema.
    TypeAST* resolvedType = nullptr;

    /// True if this expression can appear on the left of an assignment.
    /// Set by Sema.
    bool isLValue = false;

    /// True if this expression was folded to a compile-time constant.
    /// Set by the constant evaluator; never changes after that.
    bool isConst = false;

    /// The folded constant value, if `isConst` is true.
    /// Invariant: `isConst == true` iff `constValue.isEvaluated()`.
    ConstantValue constValue;

    explicit ExprAST(ASTKind k) : BaseAST(k) {}

    bool hasType() const { return resolvedType != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An attribute attached to a declaration.
///
/// An attribute is `@name` or `@name(args...)`. Attributes are
/// juxtaposed: a declaration may be preceded by any number of them, and
/// the parser collects them into a span.
///
/// The attribute set is closed (see the grammar §9). The parser does
/// not validate that the name is one of the known attributes; Sema does.
/// This keeps the parser from having to know the set, and keeps a typo
/// in an attribute name a name-resolution error rather than a parse
/// error.
///
/// @example
///   @export                       → name="export", args={}
///   @on(EventKind.KeyDown)        → name="on", args=[Direction.Member ref]
///   @capped(1000)                 → name="capped", args=[Int(1000)]
///   @deprecated("use new")        → name="deprecated", args=[String("use new")]
struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    InternedString name;

    /// Attribute arguments, if any. The grammar restricts attribute
    /// arguments to literals and dotted identifiers; the parser enforces
    /// this by producing only `LiteralExprAST`, `IdentifierExprAST`, or
    /// `FieldAccessExprAST` nodes here.
    ArenaSpan<ExprAST*> args;

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ValueDeclAST — base for declarations that bind a name to a value
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base for declarations that introduce a value binding.
///
/// In the redesigned grammar, this is `VarDeclAST` (a `let`/`const`
/// binding), `FnDeclAST` (a `FN` declaration), and `ParamAST` (a
/// function parameter). `TableDeclAST` is not a value declaration; a
/// table is a type, and it lives in the type namespace.
struct ValueDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ValueDecl;

    /// The declared type of the binding. Set by the parser for
    /// declarations that write their type, and by Sema for those that
    /// infer it. Never null after Sema completes.
    TypeAST* type = nullptr;

    /// True if the binding is immutable (`const` or a `FN` parameter
    /// with `const`). A mutable binding is a `let` variable or a plain
    /// parameter. A `const` binding cannot be reassigned and no mutation
    /// through it is allowed.
    bool isConst = false;

    /// What kind of heap resource this binding owns, if any. Set once by
    /// Sema when the declaration's type is resolved; read by every later
    /// pass that needs to know the binding's ownership behavior.
    ResourceKind resourceKind = ResourceKind::None;

    explicit ValueDeclAST(ASTKind k, InternedString n, TypeAST* t, bool isConst)
        : DeclAST(k, n), type(t), isConst(isConst) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeDeclAST — base for declarations that define a type
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base for declarations that define a type.
///
/// In the redesigned grammar, this is `TableDeclAST` — a table is a
/// type, and its declaration introduces a name into the type namespace.
/// There are no other type declarations.
struct TypeDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeDecl;

    explicit TypeDeclAST(ASTKind k, InternedString n) : DeclAST(k, n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ModuleAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The root of a parsed file.
///
/// A file is a module. The module's identity is its file path relative
/// to the package root.
///
/// The `decls` span holds the module's top-level declarations in source
/// order. Import declarations are also in this span; they are read by
/// the CLI's import linker.
struct ModuleAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Module;

    InternedString filePath;
    ArenaSpan<DeclAST*> decls;

    /// Resolved imports, keyed by alias. Populated by the CLI's
    /// import-linking step after all modules are parsed.
    std::unordered_map<InternedString, ModuleAST*> resolvedImports;

    /// True if the parse produced any errors. Mirrors the diagnostic
    /// engine's state at the moment the parse finished. Callers detect
    /// failure by checking this flag, not by checking for null.
    bool hasErrors = false;

    ModuleAST() : BaseAST(ASTKind::Module) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Unknown* — error-recovery nodes
// ─────────────────────────────────────────────────────────────────────────────
//
// A parser error-recovery path that cannot produce a real node produces
// an `Unknown*AST` with `hasSyntaxError = true`. Sema recognizes these
// and skips the construct they appear in rather than reporting a
// follow-on error for every field access on the bad node.

/// @brief A generic unknown node, used when the kind is ambiguous.
struct UnknownAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Unknown;
    UnknownAST() : BaseAST(ASTKind::Unknown) { hasSyntaxError = true; }
};

inline bool isUnknown(BaseAST* node) {
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
    UnknownDeclAST()
        : DeclAST(ASTKind::UnknownDecl, InternedString{}) {
        hasSyntaxError = true;
    }
};

struct UnknownExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownExpr;
    UnknownExprAST() : ExprAST(ASTKind::UnknownExpr) { hasSyntaxError = true; }
};

struct UnknownStmtAST : StmtAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownStmt;
    UnknownStmtAST() : StmtAST(ASTKind::UnknownStmt) { hasSyntaxError = true; }
};

struct UnknownTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::UnknownType;
    UnknownTypeAST() : TypeAST(ASTKind::UnknownType) { hasSyntaxError = true; }
};