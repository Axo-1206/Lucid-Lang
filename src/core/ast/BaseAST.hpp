/// @file BaseAST.hpp
/// 
/// @responsibility The Foundation. Defines BaseAST, the visitor-free
///                 `isa`/`as` helpers, and common types
///                 (DocComment, SourceLocation, ASTKind, ConstantValue).
/// 
/// @architectural_note
///   This file uses Forward Declarations for all AST families (Expr, Stmt,
///   Decl, Type). NEVER include a family header (like ExprAST.hpp) here;
///   this keeps the dependency graph acyclic.
/// 
/// @related_files
///   - src/ast/ExprAST.hpp, StmtAST.hpp, DeclAST.hpp, TypeAST.hpp
///   - Each family header includes BaseAST.hpp, not the other way around.
/// 
/// ============================================================================
/// FIELD CATEGORIES
/// ============================================================================
/// 
/// AST fields are organized into four categories based on who sets them and
/// when:
/// 
/// | Category        | Mutability          | Set By  | Examples                                    |
/// | --------------- | ------------------- | ------- | ------------------------------------------- |
/// | Parser Fields   | `const` (immutable) | Parser  | `name`, `type`, `init`, `body`              |
/// | Semantic Fields | `mutable`           | Sema    | `resolvedType`, `constValue`, `isLValue`    |
/// | Layout Fields   | `mutable`           | Sema    | `fieldIndex`, `moduleFieldIndex`            |
/// | CodeGen Fields  | `mutable`           | CodeGen | (none — see below)                          |
/// 
/// ## Layout Fields vs CodeGen Fields
/// 
/// Layout fields (`fieldIndex`, `moduleFieldIndex`) are computed by Sema
/// during semantic analysis. They represent decisions about memory layout
/// and symbol identity that are independent of the target machine.
/// 
/// CodeGen fields (`llvmType`, `llvmFunction`, `llvmAlloca`) are created
/// during IR lowering. They are actual LLVM IR objects that don't exist until
/// CodeGen runs. The AST holds NONE of them — every LLVM-level fact lives on
/// a codegen-side object (a `Types` cache, a `FunctionState`, a
/// `ProgramState`). The reason is that the AST outlives any single codegen
/// run: the interpreter lowers the same AST against new targets on hot
/// reload, so a cached LLVM fact on the AST would be a snapshot of the wrong
/// run.
/// 
/// This separation allows:
///   1. Sema to validate layout decisions (e.g., no self-referential structs)
///   2. CodeGen to focus on IR generation without recomputation
///   3. Clear ownership of each field's lifecycle

#pragma once

#include "../SourceLocation.hpp"
#include "../memory/ASTArena.hpp"
#include "../memory/InternedString.hpp"
#include "../memory/ArenaSpan.hpp"
#include "../diagnostics/StackTrace.hpp"
#include "ResourceKind.hpp"

#include <string>
#include <optional>
#include <memory>
#include <variant>
#include <vector>
#include <unordered_map>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations — every AST family forward-declared here so any header
// can accept a visitor or hold a pointer without pulling in the full family.
//
// The actual struct definitions live in their own headers:
//   TypeAST.hpp     — PrimitiveTypeAST, NamedTypeAST, ArrayTypeAST, ...
//   DeclAST.hpp     — FuncDeclAST, StructDeclAST, TraitDeclAST, ...
//   ExprAST.hpp     — LiteralExprAST, CallExprAST, PipelineExprAST, ...
//   StmtAST.hpp     — BlockStmtAST, ForStmtAST, ...
// ─────────────────────────────────────────────────────────────────────────────

// TypeAST.hpp
struct PrimitiveTypeAST;
struct NamedTypeAST;
struct ArrayTypeAST;
struct NullableTypeAST;
struct FallibleTypeAST;
struct CombinedTypeAST;
struct RefTypeAST;
struct FuncTypeAST;

// DeclAST.hpp
struct ImportDeclAST;
struct VarDeclAST;
struct ParamAST;
struct GenericParamDeclAST;
struct FuncDeclAST;
struct FieldDeclAST;
struct StructDeclAST;
struct EnumVariantAST;
struct EnumDeclAST;
struct TraitFieldDeclAST;
struct TraitRequireDeclAST;
struct TraitDeclAST;
struct SatisfyDeclAST;
struct DefDeclAST;
struct HostTypeDeclAST;
struct TypeAliasDeclAST;
struct StaticFnDeclAST;

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
struct SliceExprAST;
struct FieldAccessExprAST;
struct ModuleAccessExprAST;
struct NullCoalesceExprAST;
struct AssignExprAST;
struct PipelineExprAST;
struct PipelineStepAST;
struct AnonFuncExprAST;
struct IfExprAST;
struct RangeExprAST;
struct CaseValueAST;

// StmtAST.hpp
struct AwaitStmtAST;
struct SpawnStmtAST;
struct StartStmtAST;
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

// Root
struct ModuleAST;

// Special
struct ValueDeclAST;
struct TypeDeclAST;

// Unknown nodes (parser error recovery)
struct UnknownDeclAST;
struct UnknownExprAST;
struct UnknownStmtAST;
struct UnknownTypeAST;

// Compiler directive nodes
struct AttributeAST;

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

    // Special bases
    ValueDecl,
    TypeDecl,

    // Type nodes
    PrimitiveType,
    NamedType,
    ArrayType,
    NullableType,
    FallibleType,
    CombinedType,      // T?!
    RefType,
    FuncType,

    // Declaration nodes
    ImportDecl,
    VarDecl,
    Param,
    GenericParamDecl,
    FuncDecl,
    FieldDecl,
    StructDecl,
    EnumVariant,
    EnumDecl,
    TraitFieldDecl,
    TraitDecl,
    TraitRequireDecl,   // REQUIRE clause inside a trait
    SatisfyDecl,        // satisfy block
    DefDecl,            // DEF declaration
    HostTypeDecl,       // TYPE X = #host(...) / #native(...) / #builtin(...)
    TypeAliasDecl,      // TYPE X = Y (alias)
    StaticFnDecl,       // static member function inside a struct

    // Expression nodes
    LiteralExpr,
    ArrayLiteralExpr,
    StructLiteralExpr,
    FieldInit,
    IdentifierExpr,
    FieldAccessExpr,
    ModuleAccessExpr,
    CallExpr,
    IndexExpr,
    SliceExpr,
    BinaryExpr,
    UnaryExpr,
    AssignExpr,
    NullCoalesceExpr,
    PipelineExpr,
    PipelineStep,
    AnonFuncExpr,
    IfExpr,
    RangeExpr,
    CaseValue,          // one value + optional binding inside a switch case

    // Concurrency statements
    AwaitStmt,
    SpawnStmt,
    StartStmt,

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

    // Root
    Program,

    // Compiler directives
    Attribute,
};

// ─────────────────────────────────────────────────────────────────────────────
// DocComment — documentation attached to declarations only.
// ─────────────────────────────────────────────────────────────────────────────

enum class DocCommentForm {
    Stacked,   // consecutive '--' lines above declaration
    Block,     // /-- ... --/ block above declaration
    Trailing,  // '--' comment on same line as declaration
};

struct DocComment {
    InternedString  text;   // Markdown content, with the ' -' prefix stripped
    DocCommentForm  form;
};

// ─────────────────────────────────────────────────────────────────────────────
// AST_ASSERT_MSG — invariant check for compiler bugs.
//
// Unlike assert(), this:
//   - always fires (not disabled by NDEBUG), because a violated AST invariant
//     means the compiler is broken, not the user's program
//   - prints file, line, function, and a caller-supplied message
//   - prints a stack trace before aborting
//
// Use ONLY for conditions that indicate a bug in the compiler itself.
// User-facing diagnostics still go through DiagnosticEngine.
//
// Usage:
//   AST_ASSERT_MSG(kind == T::staticKind,
//                  "ASTKind mismatch in as<T>()");
//
// The message is a string literal — no allocation, no formatting.
// ─────────────────────────────────────────────────────────────────────────────
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
// BaseAST — root of the entire AST hierarchy.
// ─────────────────────────────────────────────────────────────────────────────

struct BaseAST {
    ASTKind kind;
    SourceLocation loc;
    bool hasSyntaxError = false;

    explicit BaseAST(ASTKind k) : kind(k) {}
    virtual ~BaseAST() = default;

    // ─── Type Checking ──────────────────────────────────────────────────────

    template<typename T>
    bool isa() const { return kind == T::staticKind; }

    template<typename T>
    T* as() {
        AST_ASSERT_MSG(kind == T::staticKind,
                    "ASTKind mismatch in as<T>() - caller assumed the wrong node type");
        return static_cast<T*>(this);
    }

    template<typename T>
    const T* as() const {
        AST_ASSERT_MSG(kind == T::staticKind,
                    "ASTKind mismatch in as<T>() - caller assumed the wrong node type");
        return static_cast<const T*>(this);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Family bases
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base for all statement nodes.
struct StmtAST : BaseAST {
    explicit StmtAST(ASTKind k) : BaseAST(k) {}
};

/// @brief Base for all declaration nodes.
/// 
/// Every declaration has:
///   - a name (interned string),
///   - an optional doc comment,
///   - an attribute list,
///   - a visibility flag (`isExported`),
///   - a reference to its declaring module,
///   - a declaration order within the module (for deterministic
///     initialization of module-level state).
struct DeclAST : BaseAST {
    std::optional<DocComment> doc;
    ArenaSpan<AttributeAST*>  attributes;
    const InternedString      name;

    /// @brief True if the declaration is visible outside its module.
    /// Set by Sema when it resolves the declaration's `@[export]` attribute.
    bool isExported = false;

    /// @brief The module this declaration belongs to. Set by Sema during
    /// module registration; used for mangled-name computation and for
    /// diagnostics that need to know which module a name came from.
    ModuleAST* declaringModule = nullptr;

    /// @brief Declaration order within the module. Used for deterministic
    /// initialization of module-level state (fields of the module instance
    /// struct are ordered by this value). The module's own identity is
    /// already encoded in the mangled name.
    int orderInModule = 0;

    explicit DeclAST(ASTKind k, InternedString n) : BaseAST(k), name(n) {}
    bool hasDoc() const { return doc.has_value(); }
};

/// @brief Base for all type-annotation nodes.
struct TypeAST : BaseAST {
    explicit TypeAST(ASTKind k) : BaseAST(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// CONSTANT EVALUATION DESIGN
// ─────────────────────────────────────────────────────────────────────────────
//
// Lucid evaluates constant expressions (e.g., `2 + 3`, `if true ?? 5 else 10`)
// during semantic analysis. The result is stored as metadata on the original
// AST node, not by replacing the expression subtree with a literal.
//
// ## Why Metadata, Not Replacement?
//
// 1. **Memory efficiency**: Replacing AST nodes during Sema would require
//    allocating new nodes (via the arena) for every constant expression.
//    The original AST is already allocated; reusing it avoids extra memory
//    pressure and fragmentation.
//
// 2. **Source fidelity**: Preserving the original AST is essential for
//    diagnostics. When an error occurs, we can report it in terms of the
//    original source expression, not a transformed one.
//
// 3. **Non-destructive analysis**: Other Sema passes may need to traverse
//    the original expression tree (for type checking, narrowing, capture
//    analysis). Replacing the AST would break these passes.
//
// 4. **Lazy evaluation**: The constant value is computed once and reused
//    wherever needed, without modifying the AST.
//
// ## Implementation Fields (on ExprAST)
//
//   - `isConst` : bool
//         True if the expression has been evaluated to a compile-time
//         constant. Set by `ConstEvaluator`; never changes after that.
//
//   - `constValue` : ConstantValue
//         The evaluated constant value (if `isConst` is true).
//
//   - `valueState` : ValueState
//         Reflects the result's nullability/fallibility state (Definite,
//         Nil, Err, Unknown, None). Helps with flow-sensitive narrowing.
//
//   - `resolvedType` : TypeAST*
//         The semantic type of the expression, set during type resolution.
//
// ## Usage Guidelines
//
// ### Semantic Analysis (Sema)
//   - Call `ConstEvaluator::evaluate(ctx, expr, targetType)` to evaluate an
//     expression. It returns a `ConstantValue` and sets `isConst`/`constValue`
//     on the node if successful.
//   - Use `expr->isConst` to check if a constant is available.
//   - Access the evaluated value via `expr->constValue`.
//   - Do not modify the AST structure; use the metadata fields.
//
// ### Code Generation (CodeGen)
//   - If `expr->isConst` is true, emit the constant directly.
//   - Otherwise, emit the expression as usual.
//
// ### Diagnostics
//   - When reporting an error, refer to the original expression's source
//     location (`expr->loc`) and, if helpful, include the evaluated constant
//     value in the message.
//
// ## Important Note
//
// The const evaluator never replaces the original AST node with a literal.
// The original structure remains intact for diagnostics and other passes.
// Metadata fields are the only addition.
//
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The result of compile-time constant evaluation.
/// 
/// A default-constructed `ConstantValue` has `Kind::Unknown`, which
/// `isEvaluated()` reports as false. The invariant `isConst ==
/// constValue.isEvaluated()` is maintained because only one place writes both
/// fields together (the const evaluator's post-amble).
struct ConstantValue {
    enum class Kind : uint8_t {
        Unknown,    ///< Not yet evaluated
        Error,      ///< Evaluation failed
        Void,       ///< No value (void function)
        Bool,       ///< true / false
        Int,        ///< Integer (any size)
        Float,      ///< Floating point (any precision)
        String,     ///< String literal
        Char,       ///< Character literal
        Enum,       ///< Enum variant
        Struct,     ///< Struct value
        Array,      ///< Array value
        Function,   ///< Const function pointer (for later calls)
        Nil,        ///< nil sentinel
        Err,        ///< err sentinel
    };

    Kind kind = Kind::Unknown;
    TypeAST* type = nullptr;

    /// @brief Value storage. Using a variant to hold different value types
    ///        efficiently, avoiding dynamic allocation for scalars.
    ///
    /// NOTE: The `FuncDeclAST*` case requires `FuncDeclAST` to be a complete
    /// type at the point the variant is instantiated. This file only
    /// forward-declares it. Any translation unit that actually stores a
    /// function value in a `ConstantValue` must include `DeclAST.hpp` first.
    /// This is the existing convention and is not a change.
    std::variant<
        std::monostate,                                      // Unknown, Error, Void
        bool,                                                // Bool
        int64_t,                                             // Int
        double,                                              // Float
        InternedString,                                      // String, Char, Enum
        std::vector<ConstantValue>,                          // Array
        std::unordered_map<InternedString, ConstantValue>,   // Struct
        FuncDeclAST*                                         // Function
    > value;

    // ─── Constructors ──────────────────────────────────────────────────

    ConstantValue() : kind(Kind::Unknown) {}
    explicit ConstantValue(bool v)                : kind(Kind::Bool),   value(v) {}
    explicit ConstantValue(int64_t v)             : kind(Kind::Int),    value(v) {}
    explicit ConstantValue(double v)              : kind(Kind::Float),  value(v) {}
    explicit ConstantValue(InternedString v)      : kind(Kind::String), value(v) {}
    explicit ConstantValue(FuncDeclAST* f)        : kind(Kind::Function), value(f) {}

    // ─── Factory Methods ──────────────────────────────────────────────

    static ConstantValue nil() {
        ConstantValue v; v.kind = Kind::Nil; return v;
    }
    static ConstantValue err() {
        ConstantValue v; v.kind = Kind::Err; return v;
    }
    static ConstantValue error() {
        ConstantValue v; v.kind = Kind::Error; return v;
    }
    static ConstantValue voidValue() {
        ConstantValue v; v.kind = Kind::Void; return v;
    }
    static ConstantValue unknown() {
        return ConstantValue();
    }

    // ─── Predicates ────────────────────────────────────────────────────

    bool isEvaluated() const {
        return kind != Kind::Unknown && kind != Kind::Error;
    }
    bool isError() const { return kind == Kind::Error; }
    bool isUnknown() const { return kind == Kind::Unknown; }

    bool isBool()     const { return kind == Kind::Bool; }
    bool isInt()      const { return kind == Kind::Int; }
    bool isFloat()    const { return kind == Kind::Float; }
    bool isString()   const { return kind == Kind::String; }
    bool isChar()     const { return kind == Kind::Char; }
    bool isVoid()     const { return kind == Kind::Void; }
    bool isFunction() const { return kind == Kind::Function; }
    bool isNil()      const { return kind == Kind::Nil; }
    bool isErr()      const { return kind == Kind::Err; }
    bool isStruct()   const { return kind == Kind::Struct; }
    bool isArray()    const { return kind == Kind::Array; }
    bool isEnum()     const { return kind == Kind::Enum; }

    // ─── Accessors ────────────────────────────────────────────────────

    bool         asBool()     const { return std::get<bool>(value); }
    int64_t      asInt()      const { return std::get<int64_t>(value); }
    double       asFloat()    const { return std::get<double>(value); }
    InternedString asString() const { return std::get<InternedString>(value); }
    FuncDeclAST* asFunction() const { return std::get<FuncDeclAST*>(value); }

    const std::vector<ConstantValue>& asArray() const {
        return std::get<std::vector<ConstantValue>>(value);
    }
    const std::unordered_map<InternedString, ConstantValue>& asStruct() const {
        return std::get<std::unordered_map<InternedString, ConstantValue>>(value);
    }

    std::vector<ConstantValue>& asArrayMut() {
        return std::get<std::vector<ConstantValue>>(value);
    }
    std::unordered_map<InternedString, ConstantValue>& asStructMut() {
        return std::get<std::unordered_map<InternedString, ConstantValue>>(value);
    }

    // ─── Comparison ───────────────────────────────────────────────────

    bool operator==(const ConstantValue& other) const {
        if (kind != other.kind) return false;
        if (type != other.type) return false;
        return value == other.value;
    }
    bool operator!=(const ConstantValue& other) const {
        return !(*this == other);
    }
};

/// @brief Reflects the nullability/fallibility state of an expression.
/// 
/// Used by the flow-sensitive narrowing machinery to track whether a value
/// is currently definite, `nil`, `err`, unknown, or `None` (void-returning
/// call).
enum class ValueState {
    None,       // A call that returns no value
    Definite,   // Produces a definite value (T)
    Nil,        // Produces nil (T?)
    Err,        // Produces err (T!)
    Unknown,    // Unknown at compile time (needs runtime evaluation)
};

// ─────────────────────────────────────────────────────────────────────────────
// ExprAST — base class for all expression nodes.
// ─────────────────────────────────────────────────────────────────────────────

struct ExprAST : BaseAST {
    // ─── Semantic Fields (set by Sema) ──────────────────────────────────

    /// @brief The resolved type of this expression.
    TypeAST* resolvedType = nullptr;

    /// @brief The nullability/fallibility state, for narrowing.
    ValueState valueState = ValueState::Unknown;

    /// @brief True if this expression can appear on the left of an assignment.
    bool isLValue = false;

    /// @brief True if this expression was folded to a compile-time constant.
    bool isConst = false;

    /// @brief The folded constant value, if `isConst` is true.
    /// Invariant: `isConst == true` iff `constValue.isEvaluated()`.
    ConstantValue constValue;

    explicit ExprAST(ASTKind k) : BaseAST(k) {}
    bool hasType() const { return resolvedType != nullptr; }

    // Convenience predicates for `valueState`.
    bool isNone()     const { return valueState == ValueState::None; }
    bool isDefinite() const { return valueState == ValueState::Definite; }
    bool isNil()      const { return valueState == ValueState::Nil; }
    bool isErr()      const { return valueState == ValueState::Err; }
    bool isUnknown()  const { return valueState == ValueState::Unknown; }
};

// ─────────────────────────────────────────────────────────────────────────────
// AttributeAST — represents an attribute attached to a declaration.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents an attribute attached to a declaration.
///
/// Attributes are compiler directives that provide additional information to
/// the compiler. Each attribute has a name and an optional list of literal
/// arguments. The attribute set is closed; see the grammar's Attributes
/// section for the valid names.
///
/// @note Arguments are restricted to literals only (no expressions). The
///       parser enforces this restriction by parsing `LiteralExprAST`.
///
/// @example
///   @[export]                      → name="export", args={}
///   @[deprecated("use new")]       → name="deprecated", args=[String("use new")]
///   @[opaque]                      → name="opaque", args={}
struct AttributeAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Attribute;

    InternedString name;
    ArenaSpan<LiteralExprAST*> args;

    AttributeAST() : BaseAST(ASTKind::Attribute) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ValueDeclAST — base for declarations that produce values.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Distinguishes between mutable and immutable declarations.
/// 
/// - `Let`:   mutable binding (can be reassigned)
/// - `Const`: immutable binding (cannot be reassigned)
/// 
/// @note For struct fields, `Const` means the field cannot be reassigned
///       after construction, even if the containing variable is `let`.
enum class DeclKeyword {
    Let,    // mutable
    Const   // immutable
};

/// @brief Base class for declarations that produce values.
/// 
/// Value declarations live in the VALUE NAMESPACE. When an identifier is
/// resolved in an expression context, the lookup searches this namespace
/// first.
/// 
/// Value declarations include:
///   - Variables (`VarDeclAST`)
///   - Functions (`FuncDeclAST`)
///   - Parameters (`ParamAST`)
///   - Fields (`FieldDeclAST`)
///   - Enum variants (`EnumVariantAST`)
///   - Static members (`StaticFnDeclAST`)
///   - DEF declarations (`DefDeclAST`)
/// 
/// ─── Const-ness ─────────────────────────────────────────────────────────
/// The `keyword` field determines whether this value can be mutated:
///   - `DeclKeyword::Let`:  mutable (can be reassigned)
///   - `DeclKeyword::Const`: immutable (cannot be reassigned)
/// 
/// For enum variants, the keyword is always `Const` (they are immutable
/// constants).
/// 
/// ─── Type Resolution ────────────────────────────────────────────────────
/// The `type` field stores the fully resolved type of this declaration. This
/// is set during semantic analysis and is used by expression resolvers when
/// an identifier references this declaration.
/// 
/// @note `ValueDeclAST` nodes are stored in `Scope::values` map.
struct ValueDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::ValueDecl;

    const DeclKeyword keyword;
    TypeAST* type = nullptr;

    /// @brief What kind of heap resource this binding owns, if any.
    ///
    /// ─── The Contract ─────────────────────────────────────────────────────
    /// This field is written exactly once, by Sema, at the moment the
    /// declaration's type is resolved. It is read by CodeGen at every
    /// allocation and every free site. The writer is
    /// `SemaContext::classifyResourceKind`, and it is the only writer.
    ///
    /// Every `ValueDeclAST` — every `VarDeclAST`, `ParamAST`, `FuncDeclAST`,
    /// `FieldDeclAST`, `EnumVariantAST` — has this field populated before
    /// CodeGen runs. A debug-build assertion in `generate()` walks every
    /// module's declarations and asserts each `ValueDeclAST` was
    /// classified. A binding that reaches CodeGen with its default `None`
    /// when it should have been classified is a Sema bug, and the assertion
    /// catches it at the boundary rather than as a codegen miscompile.
    ///
    /// ─── Why On Declarations, Not Expressions ─────────────────────────────
    /// An expression's resource kind is derived on demand, from its resolved
    /// type, at the point of use — `Ownership::drop(expr->resolvedType,
    /// value)`. It is not cached on the expression, because an expression
    /// has no lifetime: it is evaluated, its value is consumed or stored,
    /// and it is gone. Every use site that needs the classification computes
    /// it from the type it already has.
    ///
    /// A declaration *does* have a lifetime — scope entry to scope exit — and
    /// its binding owns a resource for the duration of that lifetime. Every
    /// use site within that lifetime must agree on what the binding owns, so
    /// the classification is cached once and read many times.
    ///
    /// `mutable` because Sema writes it after construction.
    ResourceKind resourceKind = ResourceKind::None;

    /// @brief Index of this binding's slot in its owning module's instance
    ///        struct, or `SIZE_MAX` if the binding is not module-level.
    ///
    /// Set once by Sema, in `registerTopLevelNames`, for every top-level
    /// `VarDeclAST` (and every top-level `cls`-shaped `FuncDeclAST`). Read
    /// by CodeGen when emitting `__init_module_<name>`,
    /// `__free_module_<name>`, and every module-level access.
    ///
    /// This is a Layout Field: set by Sema, read by later passes, describing
    /// the memory layout of module state. Under the module-as-namespace
    /// model there is no per-variable `GlobalVariable`; the variable's
    /// storage is a field in the module instance, and this index is how
    /// CodeGen finds it.
    ///
    /// Invariant: at most one `ValueDeclAST*` per module has any given
    /// index. Non-module-level bindings (locals, params, fields) have
    /// `SIZE_MAX`.
    size_t moduleFieldIndex = SIZE_MAX;

    bool isModuleLevel() const { return moduleFieldIndex != SIZE_MAX; }
    bool isConst() const { return keyword == DeclKeyword::Const; }
    bool isLet()   const { return keyword == DeclKeyword::Let; }

    explicit ValueDeclAST(ASTKind k, InternedString n, DeclKeyword kw, TypeAST* t)
        : DeclAST(k, n), keyword(kw), type(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeDeclAST — base for declarations that define types.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Base class for declarations that define types.
/// 
/// Type declarations live in the TYPE NAMESPACE. When an identifier is
/// resolved in a type-annotation context, the lookup searches this namespace.
/// 
/// Type declarations include:
///   - Structs (`StructDeclAST`)
///   - Enums (`EnumDeclAST`)
///   - Traits (`TraitDeclAST`)
///   - Host-backed types (`HostTypeDeclAST`)
///   - Type aliases (`TypeAliasDeclAST`)
/// 
/// @note `TypeDeclAST` nodes are stored in `Scope::types` map.
struct TypeDeclAST : DeclAST {
    static constexpr ASTKind staticKind = ASTKind::TypeDecl;

    explicit TypeDeclAST(ASTKind k, InternedString n) : DeclAST(k, n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ModuleAST — root node for a single translation unit.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Root node for a source file module.
/// 
/// A `ModuleAST` represents the entire parsed contents of a single `.luc`
/// file.
/// 
/// ─── File Path and Module Identity ──────────────────────────────────────
/// A file is a module; the file's path relative to the package root is the
/// module's identity. There is no in-file declaration that names the module.
/// `filePath` is the resolved canonical path; it is the stable key for
/// dependency tracking and module lookup.
/// 
/// ─── Imports Storage ────────────────────────────────────────────────────
/// The `imports` field stores the user-written module paths of all imported
/// modules. This is what the source actually wrote; the resolved
/// `ModuleAST*` pointers for those imports are stored in `resolvedImports`,
/// keyed by the alias (or the last path segment if no alias was written).
/// 
/// ─── Semantic Fields ────────────────────────────────────────────────────
/// The `specializations` vector holds the generic instantiations built during
/// resolution of *this module's* declarations. A specialization is created
/// once per distinct (template, args) pair and shared by every module that
/// references it. The *declaration* is unique; what needs a per-module home
/// is the question "which module should emit the LLVM type or function for
/// this?" — the module whose resolution first triggered the instantiation.
///
/// Push order is completion order during resolution. Since Sema resolves
/// depth-first and a specialization is only appended after its own body has
/// been fully resolved, the vector is already in dependency order: a
/// specialization's dependencies appear before it. CodeGen can lower the
/// vector front-to-back without a topological sort.
struct ModuleAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Program;

    InternedString filePath;
    ArenaSpan<DeclAST*> decls;
    bool hasErrors = false;

    /// @brief Resolved imports, keyed by alias (or last path segment).
    std::unordered_map<InternedString, ModuleAST*> resolvedImports;

    /// @brief User-written import paths, in declaration order.
    std::vector<InternedString> imports;

    /// @brief Dependency order in the topological sort of imports.
    int dependencyOrder = -1;

    /// @brief Specializations built during this module's resolution.
    std::vector<DeclAST*> specializations;

    ModuleAST() : BaseAST(ASTKind::Program) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericParamDeclAST — a generic type parameter declaration.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a single generic type parameter declaration.
/// 
/// This node appears in the generic parameter list of functions, structs,
/// enums, traits, and host-backed types. Each parameter has a name and an
/// optional list of trait constraints.
/// 
/// @par Grammar Reference
///   generic_param := IDENTIFIER
///                  | IDENTIFIER ':' trait_ref { '+' trait_ref }
/// 
/// @par Examples
///   @code
///   struct Box<T> { ... }                         // unconstrained T
///   const magnitude<T : Vector2> (v T) -> float   // T must satisfy Vector2
///   struct Pair<A : Named, B : Named> { ... }     // two constrained parameters
///   TYPE Map<K : Eq, V> = #host(LucidMap)         // constrained host type
///   @endcode
/// 
/// @par Semantic Resolution
///   During semantic analysis, each constraint type is resolved to a
///   `TraitDeclAST`. The order of constraints does not affect semantics,
///   but is preserved for source fidelity.
/// 
/// @field name          The identifier of the type parameter (e.g., "T").
/// @field constraints   Trait types this parameter must satisfy. Empty span
///                      means the parameter is unconstrained. Each constraint
///                      is a `NamedTypeAST` resolving to a `TraitDeclAST`.
/// 
/// @note Multiple constraints are joined with `+` in source (e.g.,
///       `T : Vector2 + Named`).
struct GenericParamDeclAST : TypeDeclAST {
    static constexpr ASTKind staticKind = ASTKind::GenericParamDecl;

    ArenaSpan<NamedTypeAST*> constraints;   // empty = unconstrained

    explicit GenericParamDeclAST(InternedString n)
        : TypeDeclAST(ASTKind::GenericParamDecl, n) {}
};
using ParamGroup = std::vector<ParamAST*>;

// ─────────────────────────────────────────────────────────────────────────────
// UnknownAST family — error recovery nodes.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Generic unknown node – fallback when the specific kind is ambiguous.
/// 
/// Used only when the parser cannot determine whether the invalid syntax was
/// a declaration, expression, statement, or type. Prefer the more specific
/// unknown node types when possible.
struct UnknownAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::Unknown;
    UnknownAST() : BaseAST(ASTKind::Unknown) { hasSyntaxError = true; }
};

/// @brief True if `node` is null or one of the error-recovery nodes.
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
    UnknownDeclAST() : DeclAST(ASTKind::UnknownDecl, InternedString()) { hasSyntaxError = true; }
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

// ─────────────────────────────────────────────────────────────────────────────
// CapturedVariable — information about a variable captured by a closure.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A variable captured by a closure.
/// 
/// Each capture is a single slot in the closure's environment. The slot holds
/// either a snapshot of the captured value (for read-only captures) or a
/// reference to shared storage (for captures the closure writes to).
struct CapturedVariable {
    // ─── Lexical Identity (invariant under generic substitution) ────────
    InternedString name;

    /// The declaration this capture resolves to, in the specialized context.
    /// Set by Sema's capture analysis; valid for CodeGen because substitution
    /// rebuilds the anon before capture analysis runs on it, so the
    /// declaration pointer is never stale.
    ValueDeclAST* resolvedDecl = nullptr;

    // ─── Capture Flags (computed once by capture analysis) ─────────────
    /// True if the closure writes to the captured variable, and therefore
    /// must share one heap slot with every other holder (the enclosing
    /// frame, and any other closure capturing the same declaration). False
    /// if the closure only reads it, in which case it may be
    /// snapshot-copied into the environment at construction time.
    bool byReference = false;

    /// True if the value being captured is itself a closure — meaning the
    /// environment slot must hold a fat pointer `{ func, env }` and the
    /// closure's environment must be retained, rather than holding a bare
    /// function pointer.
    ///
    /// Set for function-typed parameters and struct fields where the actual
    /// value's shape (fn vs cls) is not known at the capture site. CodeGen
    /// emits a runtime shape check for these captures.
    bool isClosureValue = false;

    // ─── Environment Layout (set by Sema, per closure) ─────────────────
    /// Index of this capture's slot in the owning closure's environment
    /// struct. Assigned on insert; distinct for each closure that captures
    /// the same variable.
    size_t index = 0;
};