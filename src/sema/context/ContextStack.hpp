/// @file ContextStack.hpp
/// @brief Semantic context tracking - manages what we're analyzing and type narrowing.
///
/// # What This File Contains
///
/// The ContextStack tracks the current semantic state during AST analysis.
/// It answers three key questions:
///
/// 1. **Where are we?** - What context are we in (function, loop, if, switch, block)?
/// 2. **What symbols are in scope?** - Variables, types, and generic parameters.
/// 3. **What types have been narrowed?** - Flow-sensitive type refinement.
///
/// # The Three Stacks
///
/// ```
/// ┌────────────────────────────────────────────────────────────────────────────┐
/// │                          ContextStack                                      │
/// │                                                                            │
/// │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐  │
/// │  │  Context Stack   │  │ Narrowing Stack  │  │  Return Stack            │  │
/// │  │  (where are we?) │  │ (what's narrow?) │  │  (what's expected RT?)   │  │
/// │  ├──────────────────┤  ├──────────────────┤  ├──────────────────────────┤  │
/// │  │ FuncBody         │  │ { x → int }      │  │ int                      │  │
/// │  │ LoopBody         │  │ { }              │  │ (int) → int              │  │
/// │  │ IfStmt           │  │ { }              │  └──────────────────────────┘  │
/// │  │ Block            │  └──────────────────┘                                │
/// │  └──────────────────┘                                                      │
/// └────────────────────────────────────────────────────────────────────────────┘
/// ```
///
/// ## 1. Context Stack
///
/// Tracks the syntactic context for validation rules:
/// - `FuncBody`: `return` is allowed
/// - `LoopBody`: `break` and `continue` are allowed
/// - `SwitchBody`: `case` and `default` are allowed
/// - `IfStmt`: Type narrowing is being tracked
/// - `Block`: Pending inverse narrowing can be applied
///
/// ## 2. Narrowing Stack
///
/// Tracks flow-sensitive type refinements from:
/// - `if x != nil` → `T?` becomes `T` in the then branch
/// - `if x == nil` with early exit → `T?` becomes `T` in the rest of the block
/// - `await x` → `Future<T>` becomes `T`
/// - `join x` → `Thread<T>` becomes `T`
///
/// ## 3. Return Stack
///
/// Tracks expected return types for nested functions (currying support):
/// - `(a int) -> (int) -> int` has nested return types: `(int) -> int` then `int`
///
/// # Quick Reference
///
/// | What You Need                     | Method                        |
/// | --------------------------------- | ----------------------------- |
/// | Are we inside a function?         | `insideFunction()`            |
/// | Are we inside a loop?             | `insideLoop()`                |
/// | Are we inside a switch?           | `insideSwitch()`              |
/// | Are we analyzing an if condition? | `isIfConditionCtx()`          |
/// | What's the narrowed type of `x`?  | `getNarrowedType(x)`          |
/// | Current expected return type      | `currentReturnType()`         |
/// | Narrow `x` to `T`                 | `narrowVariable(x, T)`        |
/// | Push a narrowing level            | `pushNarrowingLevel()`        |
/// | Pop a narrowing level             | `popNarrowingLevel()`         |

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/SourceLocation.hpp"
#include "core/memory/InternedString.hpp"

#include <vector>
#include <unordered_map>

namespace sema {

// ─── ContextKind ──────────────────────────────────────────────────────────

/// @brief Types of semantic contexts.
enum class ContextKind : uint8_t {
    TopLevel,      ///< Module-level declarations
    FuncBody,      ///< Inside a function body (return allowed)
    LoopBody,      ///< Inside a loop body (break/continue allowed)
    SwitchBody,    ///< Inside a switch body (case/default allowed)
    IfStmt,        ///< Inside an if statement (for type narrowing)
    Block          ///< Inside a block statement (for pending inverse narrowing)
};

// ─── NarrowingInfo ──────────────────────────────────────────────────────

/// @brief Information about type narrowing from a condition or operation.
///
/// Captures the narrowing effect of:
/// - `if x != nil` → `x` narrowed to non-nullable
/// - `if x == nil` with early exit → `x` narrowed to non-nullable in rest of block
/// - `await x` → `x` narrowed from `Future<T>` to `T`
/// - `join x` → `x` narrowed from `Thread<T>` to `T`
///
/// @example
///   if x != nil { ... }  → NarrowingInfo { x→T, isEquality: false }
///   if x == nil { return } → NarrowingInfo { x→T, isEquality: true }
///   await result         → NarrowingInfo { result→T, isEquality: false }
struct NarrowingInfo {
    /// True if this struct contains valid narrowing info.
    bool hasNarrowing = false;
    
    /// Map from variable name to its narrowed type.
    std::unordered_map<InternedString, TypeAST*> narrowings;
    
    /// True for `==`, `await`, `join` (narrowing applies to the rest of block)
    /// False for `!=`, `is` checks (narrowing applies to then branch only)
    bool isEquality = false;
};
// ─── Narrowing Limitations ─────────────────────────────────────────────────

/// @note **IMPORTANT: Mixed Conditions Are Not Supported**
///
/// The current narrowing system only handles conditions where ALL checks
/// use the SAME operator:
///
/// ✅ Supported:
/// ```lucid
/// if x != nil and y != nil { ... }    -- All != checks (direct narrowing)
/// if x == nil or y == nil { ... }     -- All == checks (inverse narrowing)
/// if x != nil { ... }                 -- Single check
/// ```
///
/// ❌ NOT Supported (will be rejected):
/// ```lucid
/// if x != nil and y == nil { ... }    -- Mixed != and == in same condition
/// if x != nil or y == nil { ... }     -- Mixed != and == in same condition
/// ```
///
/// ## Why This Restriction Exists
///
/// 1. **Control Flow Ambiguity**: For `x != nil AND y == nil`, the narrowing
///    semantics are not well-defined:
///    - `x != nil` → narrows `x` in THEN branch
///    - `y == nil` → narrows `y` in ELSE branch (inverse)
///    - These contradict each other - no single `NarrowingInfo` can represent both.
///
/// 2. **`isEquality` Flag**: `NarrowingInfo` has only one `isEquality` flag.
///    Mixed conditions would require per-variable flags, which the current
///    implementation does not support.
///
/// 3. **Control Flow Graph Complexity**: Supporting mixed operators would
///    require a full control flow graph with SSA-style φ-nodes, which is
///    beyond the scope of the current narrowing implementation.
///
/// ## Workaround
///
/// Use nested if statements to handle mixed conditions:
///
/// ```lucid
/// if x != nil {
///     if y == nil {
///         return
///     }
///     // x is int, y is not nil
/// }
/// ```
///
/// ## Future Enhancement
///
/// If mixed conditions become a common need, consider:
/// - Replacing `isEquality` with a per-variable operator map
/// - Using a control flow graph for precise narrowing
/// - Adding a more sophisticated dataflow analysis

// ─── Pending Concurrency Operations ─────────────────────────────────────

/// @brief Represents a pending async operation that must be awaited.
struct PendingAsync {
    InternedString name;
    ExprAST* call;
    SourceLocation loc;
};

/// @brief Represents a pending spawn operation that must be joined.
struct PendingSpawn {
    InternedString name;
    ExprAST* call;
    SourceLocation loc;
};

// ─── Scope ──────────────────────────────────────────────────────────────

/// @brief A single lexical scope containing symbols.
///
/// Scopes are pushed when entering function bodies, blocks, if/else branches,
/// loop bodies, and switch bodies.
///
/// Each scope has three namespaces:
/// - **Values**: Variables, functions, parameters, fields, enum variants
/// - **Types**: Structs, enums, traits
/// - **Generic Parameters**: `<T>` parameters (shadow type lookups)
struct Scope {
    std::unordered_map<InternedString, ValueDeclAST*> values;
    std::unordered_map<InternedString, TypeDeclAST*> types;
    std::unordered_map<InternedString, GenericParamDeclAST*> genericParams;
    std::unordered_map<InternedString, PendingAsync> pendingAsync;
    std::unordered_map<InternedString, PendingSpawn> pendingSpawn;
};

// ─── ReturnStack ─────────────────────────────────────────────────────────

/// @brief Stack for tracking expected return types in nested functions.
///
/// For curried functions like `(a int) -> (int) -> int`, each `->` creates
/// a new function body with its own expected return type.
///
/// @example
/// ```lucid
/// const add (a int) -> (int) -> int = {
///     -- Stack: [ (int) -> int ]
///     return (b int) -> int {
///         -- Stack: [ (int) -> int, int ]
///         return a + b          -- Check: int matches int ✅
///     }                         -- Pop int
/// }                             -- Pop (int) -> int
/// ```
class ReturnStack {
public:
    void push(TypeAST* returnType) { m_stack.push_back(returnType); }
    void pop() { if (!m_stack.empty()) m_stack.pop_back(); }
    TypeAST* current() const { return m_stack.empty() ? nullptr : m_stack.back(); }
    bool empty() const { return m_stack.empty(); }
    size_t size() const { return m_stack.size(); }

private:
    std::vector<TypeAST*> m_stack;
};

// ─── ContextFrame ──────────────────────────────────────────────────────

/// @brief One frame on the context stack.
///
/// Each frame tracks a semantic construct and stores context-specific data.
struct ContextFrame {
    ContextKind kind;
    BaseAST* node = nullptr;

    // ─── Return Type (FuncBody) ──────────────────────────────────────────
    TypeAST* expectedReturnType = nullptr;

    // ─── Loop/Switch Tracking ──────────────────────────────────────────
    StmtAST* loopStmt = nullptr;
    SwitchStmtAST* switchStmt = nullptr;

    // ─── Type Narrowing (IfStmt) ────────────────────────────────────────
    bool isIfConditionCtx = false;
    bool hasElse = false;
    NarrowingInfo pendingNarrowing;

    // ─── Pending Inverse Narrowing (Block) ──────────────────────────────
    bool hasPendingInverseNarrowing = false;
    NarrowingInfo pendingInverseNarrowing;
};

// ─── ContextStack ──────────────────────────────────────────────────────

/// @brief Unified context manager for semantic analysis.
///
/// ## Type Narrowing Flow
///
/// ### Then Branch (Direct Narrowing)
/// ```lucid
/// if x != nil {    ← Condition analyzed
///     // x is int  ← ScopedNarrowing applies direct narrowing
/// }
/// ```
///
/// ### Else Branch (Inverse Narrowing)
/// ```lucid
/// if x != nil {
///     // x is int
/// } else {
///     // x is nil  ← ScopedNarrowing applies inverse narrowing
/// }
/// ```
///
/// ### Standalone If (Pending Inverse Narrowing)
/// ```lucid
/// if x == nil { return }  ← Early exit
/// // x is int             ← Applied to the rest of the block
/// ```
///
/// ### Await/Join Narrowing (Linear Types)
/// ```lucid
/// async result int = fetch()   ← result is Future<int>
/// await result                 ← Narrow Future<int> → int
/// // result is int here
/// ```
///
/// ## How Narrowing Works
///
/// 1. **Condition Analysis**: `extractNarrowingsFromCondition()` examines
///    the if condition and produces a `NarrowingInfo` map.
///
/// 2. **Then Branch**: `ScopedNarrowing` applies the narrowings directly.
///    `narrowVariable()` stores the narrowed type in the current level.
///
/// 3. **Else Branch**: Inverse narrowing is applied (e.g., `x != nil` in
///    then means `x == nil` in else).
///
/// 4. **Standalone If**: If the then branch exits (return/break/continue),
///    the inverse narrowing is stored as pending and applied when the
///    enclosing block is entered.
///
/// 5. **Lookup**: `getNarrowedType()` checks the narrowing stack first
///    before falling back to the declaration's type.
class ContextStack {
public:
    // ─── Push/Pop ────────────────────────────────────────────────────────

    void push(ContextKind kind, BaseAST* node);
    void pushFunction(FuncDeclAST* node, TypeAST* returnType);
    void pushAnonFunction(AnonFuncExprAST* node, TypeAST* returnType);
    void pushLoop(StmtAST* loopStmt);
    void pushSwitch(SwitchStmtAST* switchStmt);
    void pushBlock(BlockStmtAST* block);
    void pop();

    // ─── Context Queries ──────────────────────────────────────────────────

    ContextKind current() const;
    bool isInside(ContextKind kind) const;
    BaseAST* currentNode() const;

    bool insideFunction() const;
    bool insideLoop() const;
    bool insideSwitch() const;

    FuncDeclAST* currentFunction() const;
    StmtAST* currentLoop() const;
    SwitchStmtAST* currentSwitch() const;
    BlockStmtAST* currentBlock() const;

    // ─── Return Type Tracking ──────────────────────────────────────────

    void pushReturnType(TypeAST* returnType) { m_returnStack.push(returnType); }
    void popReturnType() { m_returnStack.pop(); }
    TypeAST* currentReturnType() const { return m_returnStack.current(); }
    bool hasReturnRequirements() const { return !m_returnStack.empty(); }

    // ─── Type Narrowing ──────────────────────────────────────────────────

    // ─── If Condition Context ──────────────────────────────────────────
    bool isIfConditionCtx() const;
    void setIfConditionCtx(bool isIfCtx);
    void setHasElse(bool hasElse);
    bool hasElse() const;

    // ─── Pending Narrowing (from condition) ────────────────────────────
    void setPendingNarrowing(const NarrowingInfo& info);
    const NarrowingInfo& getPendingNarrowing() const;
    void clearPendingNarrowing();

    // ─── Narrowing Stack ────────────────────────────────────────────────
    void pushNarrowingLevel(bool isInverse = false);
    void popNarrowingLevel();
    void narrowVariable(InternedString name, TypeAST* type);
    TypeAST* getNarrowedType(InternedString name) const;
    bool isNarrowingInverse() const;

    // ─── Pending Inverse Narrowing (for standalone if) ─────────────────
    void setPendingInverseNarrowing(const NarrowingInfo& info);
    bool hasPendingInverseNarrowing() const;
    const NarrowingInfo& getPendingInverseNarrowing() const;
    void clearPendingInverseNarrowing();

    // ─── Closure Helpers ──────────────────────────────────────────────────

    /// @brief Get the current closure nesting depth.
    size_t getClosureDepth() const;

    /// @brief Check if we're inside a nested function.
    bool insideNestedFunction() const;

    /// @brief Get the innermost function declaration.
    FuncDeclAST* getInnermostFunction() const;

    /// @brief Get the innermost function node (FuncDeclAST or AnonFuncExprAST).
    BaseAST* getInnermostFunctionNode() const;


private:
    // ─── Members ──────────────────────────────────────────────────────────

    /// Context stack - tracks what we're analyzing.
    std::vector<ContextFrame> m_stack;

    /// Return stack - tracks expected return types for nested functions.
    ReturnStack m_returnStack;

    /// Narrowing stack - tracks flow-sensitive type refinements.
    struct NarrowingLevel {
        std::unordered_map<InternedString, TypeAST*> narrowedTypes;
        bool isInverse = false;
    };
    std::vector<NarrowingLevel> m_narrowing;

    // ─── Helpers ──────────────────────────────────────────────────────────

    ContextFrame* findInnermostFunction();
    const ContextFrame* findInnermostFunction() const;
    ContextFrame* findInnermostIfContext();
    const ContextFrame* findInnermostIfContext() const;
    ContextFrame* findInnermostBlock();
    const ContextFrame* findInnermostBlock() const;
};

} // namespace sema