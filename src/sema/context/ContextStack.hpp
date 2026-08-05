/// @file ContextStack.hpp
/// @brief Simplified semantic context tracking - monolithic design.
///
/// # Overview
///
/// The ContextStack tracks the current semantic context during AST analysis.
/// It answers questions like: "Are we inside a function?" or "What type was
/// this variable narrowed to?"
///
/// ## Quick Reference
///
/// | Question                                | Method                          |
/// | --------------------------------------- | ------------------------------- |
/// | Are we inside a function?               | `insideFunction()`              |
/// | Are we inside a loop?                   | `insideLoop()`                  |
/// | Are we inside an if condition?          | `isIfConditionCtx()`            |
/// | What's the narrowed type of X?          | `getNarrowedType(X)`            |
/// | What's the current expected return type?| `currentReturnType()`           |
/// | Is a type being defined?                | `isDefiningType(T)`             |
///
/// # Return Stack
///
/// The ReturnStack is a simple stack-based mechanism for tracking expected
/// return types in nested function bodies. It replaces the complex
/// ReturnRequirements group/level system with a clean push/pop model.
///
/// ## Why a Stack?
///
/// For curried functions like `(a int) -> (int) -> int`, each `->` creates
/// a new function body with its own expected return type:
///
/// ```lucid
/// const add (a int) -> (int) -> int = {
///     -- Outer body: expected return type is (int) -> int
///     return (b int) -> int {
///         -- Inner body: expected return type is int
///         return a + b
///     }
/// }
/// ```
///
/// The stack naturally models this nesting:
/// ```
/// Enter outer function  → push (int) -> int
/// Enter inner function  → push int
/// Return in inner body  → check against top of stack (int) ✅
/// Exit inner function   → pop int
/// Return in outer body  → check against top of stack ((int) -> int) ✅
/// Exit outer function   → pop (int) -> int
/// ```
///
/// ## Stack Lifecycle
///
/// ```
/// resolveFuncDecl()
///   │
///   ├─ pushReturnType(funcType->returnType)
///   │
///   ├─ resolveBlock(body)
///   │    │
///   │    ├─ resolveReturnStmt()
///   │    │    │
///   │    │    ├─ expectedType = currentReturnType()
///   │    │    ├─ resolveExprWithTarget(returnValue, expectedType)
///   │    │    └─ ...
///   │    │
///   │    └─ resolveBlock(innerFunction.body)
///   │         │
///   │         ├─ resolveAnonFuncExpr()
///   │         │    │
///   │         │    ├─ pushReturnType(innerFuncType->returnType)
///   │         │    ├─ resolveBlock(innerBody)
///   │         │    │    └─ resolveReturnStmt() → checks against inner type
///   │         │    └─ popReturnType()
///   │         │
///   │         └─ ...
///   │
///   └─ popReturnType()
/// ```
///
/// ## Example: Curried Function with Multiple Levels
///
/// ```lucid
/// const build (a int) -> (int) -> (int) -> int = {
///     return (b int) -> (int) -> int {
///         return (c int) -> int {
///             return a + b + c
///         }
///     }
/// }
/// ```
///
/// Stack evolution:
/// ```
/// Level 0: Enter build        → push (int) -> (int) -> int
/// Level 1: Enter outer return → push (int) -> int
/// Level 2: Enter inner return → push int
/// Level 2: Return c           → check int ✅ → pop int
/// Level 1: Return function    → check (int) -> int ✅ → pop (int) -> int
/// Level 0: Return function    → check (int) -> (int) -> int ✅ → pop (int) -> (int) -> int
/// ```
///
/// ## Error Cases
///
/// The ReturnStack catches type mismatches at compile time:
///
/// ```lucid
/// const bad (a int) -> int = {
///     return "hello"  -- ❌ ERROR: expected int, got string
/// }
/// ```
///
/// ```
/// resolveReturnStmt:
///   1. expectedType = currentReturnType() → int
///   2. resolveExprWithTarget("hello", int) → fails
///   3. diagnostics.error("type mismatch")
/// ```

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
/// 
/// Each frame on the context stack has one of these kinds.
/// The kind determines what statements are legal (e.g., `return` is only
/// legal inside FuncBody, `break` is only legal inside LoopBody).
enum class ContextKind : uint8_t {
    TopLevel,      ///< Module-level declarations (no function context)
    FuncBody,      ///< Inside a function body (return allowed)
    LoopBody,      ///< Inside a loop body (break/continue allowed)
    SwitchBody,    ///< Inside a switch body (case/default allowed)
    IfStmt,        ///< Inside an if statement (for type narrowing)
    Block          ///< Inside a block statement (for pending inverse narrowing)
};

// ─── NarrowingInfo ──────────────────────────────────────────────────────

/// @brief Information about type narrowing from an if condition.
/// 
/// @example
///   if x != nil { ... }  → NarrowingInfo { x→int, isEquality: false }
///   if x == nil { return }  → NarrowingInfo { x→int, isEquality: true }
/// 
/// @see extractNarrowingsFromCondition() in TypeNarrowHelpers.cpp
struct NarrowingInfo {
    /// True if this struct contains valid narrowing info.
    bool hasNarrowing = false;
    
    /// Map from variable name to its narrowed type.
    /// Example: x → int (when x was int?)
    std::unordered_map<InternedString, const TypeAST*> narrowings;
    bool isEquality = false;  // true for ==, false for !=
};

// ─── Pending Concurrency Operations ─────────────────────────────────────

/// @brief Represents a pending async operation in the current scope.
struct PendingAsync {
    InternedString name;
    const ExprAST* call;
    SourceLocation loc;
};

/// @brief Represents a pending spawn operation in the current scope.
struct PendingSpawn {
    InternedString name;
    const ExprAST* call;
    SourceLocation loc;
};

// ─── Scope ──────────────────────────────────────────────────────────────

/// @brief A single transient lexical scope.
struct Scope {
    /// Value namespace: variables, functions, parameters, fields, enum variants
    std::unordered_map<InternedString, const ValueDeclAST*> values;
    
    /// Type namespace: structs, enums, traits
    std::unordered_map<InternedString, const TypeDeclAST*> types;
    
    /// Generic parameter names (shadow type lookups)
    std::unordered_map<InternedString, const GenericParamDeclAST*> genericParams;
    
    /// Pending async operations that need to be awaited
    std::unordered_map<InternedString, PendingAsync> pendingAsync;
    
    /// Pending spawn operations that need to be joined
    std::unordered_map<InternedString, PendingSpawn> pendingSpawn;
};

// ─── ReturnStack ─────────────────────────────────────────────────────────

/// @brief Simple stack for tracking expected return types in nested functions.
/// 
/// When entering a function body, push the expected return type.
/// When exiting, pop it. Return statements check against the top of the stack.
/// 
/// @example
/// ```lucid
/// const add (a int) -> (int) -> int = {
///     -- Stack: [ (int) -> int ]
///     return (b int) -> int {
///         -- Stack: [ (int) -> int, int ]
///         return a + b
///         -- Check: a + b is int → matches top of stack (int) ✅
///     }
///     -- Check: returned function matches (int) -> int ✅
/// }
/// ```
///
/// @note This is a simple wrapper around std::vector. It provides a
///       clean interface for the semantic analyzer to push/pop return types.
class ReturnStack {
public:
    /// @brief Push an expected return type onto the stack.
    /// 
    /// Called when entering a function body.
    /// @param returnType The expected return type for this function body.
    void push(const TypeAST* returnType) {
        m_stack.push_back(returnType);
    }
    
    /// @brief Pop the top of the stack.
    /// 
    /// Called when exiting a function body.
    void pop() {
        if (!m_stack.empty()) {
            m_stack.pop_back();
        }
    }
    
    /// @brief Get the current expected return type.
    /// 
    /// @return The top of the stack, or nullptr if the stack is empty.
    const TypeAST* current() const {
        return m_stack.empty() ? nullptr : m_stack.back();
    }
    
    /// @brief Check if the stack is empty.
    bool empty() const {
        return m_stack.empty();
    }
    
    /// @brief Get the size of the stack.
    size_t size() const {
        return m_stack.size();
    }

private:
    std::vector<const TypeAST*> m_stack;
};

// ─── ContextFrame ──────────────────────────────────────────────────────

/// @brief One frame on the context stack.
/// 
/// Each frame represents a semantic construct (function, loop, if, block, etc.)
/// and stores context-specific information needed for validation.
struct ContextFrame {
    /// The kind of context.
    ContextKind kind;
    
    /// The AST node that opened this context.
    BaseAST* node = nullptr;
    
    /// Where the construct was opened (for diagnostics).
    SourceLocation openedAt;
    
    // ─── Return Type ──────────────────────────────────────────────────────
    /// @brief Expected return type for this function body.
    /// 
    /// For curried functions, this may be another FuncTypeAST.
    /// The ReturnStack manages pushing/popping these types.
    const TypeAST* expectedReturnType = nullptr;
    
    // ─── Loop/Switch Tracking ──────────────────────────────────────────
    StmtAST* loopStmt = nullptr;              ///< The loop statement
    SwitchStmtAST* switchStmt = nullptr;      ///< The switch statement
    
    // ─── Type Narrowing (only for IfStmt) ─────────────────────────────
    bool isIfConditionCtx = false;            ///< Analyzing an if condition
    bool hasElse = false;                     ///< If has an else branch
    NarrowingInfo pendingNarrowing;           ///< Narrowing from condition
    
    /// @brief Pending inverse narrowing for standalone if with early exit.
    /// 
    /// Example:
    /// ```lucid
    /// if x == nil { return }
    /// // x is int here (inverse narrowing)
    /// ```
    bool hasPendingInverseNarrowing = false;
    NarrowingInfo pendingInverseNarrowing;
};

// ─── ContextStack ──────────────────────────────────────────────────────

/// @brief Unified context manager - single class does everything.
/// 
/// # Type Narrowing Flow
/// 
/// ## Then Branch (Direct Narrowing)
/// ```lucid
/// if x != nil {    ← Condition analyzed
///     // x is int  ← Narrowing applied
/// }
/// ```
/// 
/// ## Else Branch (Inverse Narrowing)
/// ```lucid
/// if x != nil {
///     // x is int
/// } else {
///     // x is nil  ← Inverse narrowing
/// }
/// ```
/// 
/// ## Standalone If (Pending Inverse Narrowing)
/// ```lucid
/// if x == nil { return }  ← Early exit
/// // x is int             ← Inverse narrowing applied to rest of block
/// ```
/// 
/// @see analyzeIfStmt() in SemaStmt.cpp for the implementation
/// @see extractNarrowingsFromCondition() in TypeNarrowHelpers.cpp
class ContextStack {
public:
    // ─── Push/Pop ────────────────────────────────────────────────────────

    /// Push a generic context frame.
    void push(ContextKind kind, BaseAST* node, const SourceLocation& loc);
    
    /// Push a function context with expected return type.
    /// 
    /// This pushes the function context and the expected return type onto
    /// the ReturnStack. The return type is used by resolveReturnStmt to
    /// validate return values.
    /// 
    /// @param node The function declaration AST node.
    /// @param returnType The expected return type for this function body.
    /// @param loc The source location for diagnostics.
    void pushFunction(FuncDeclAST* node, const TypeAST* returnType, const SourceLocation& loc);
    
    /// Push an anonymous function context with expected return type.
    /// 
    /// Similar to pushFunction, but for anonymous function expressions.
    /// 
    /// @param node The anonymous function expression AST node.
    /// @param returnType The expected return type for this function body.
    /// @param loc The source location for diagnostics.
    void pushAnonFunction(AnonFuncExprAST* node, const TypeAST* returnType, const SourceLocation& loc);
    
    /// Push a loop context.
    void pushLoop(StmtAST* loopStmt, const SourceLocation& loc);
    
    /// Push a switch context.
    void pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc);
    
    /// Push a block context.
    void pushBlock(BlockStmtAST* block, const SourceLocation& loc);
    
    /// Pop the innermost context.
    /// 
    /// If the innermost context is a function body, it also pops the
    /// corresponding return type from the ReturnStack.
    void pop();

    // ─── Queries ──────────────────────────────────────────────────────────

    /// Get the current context kind.
    ContextKind current() const;
    
    /// Check if we're inside a specific context kind.
    bool isInside(ContextKind kind) const;
    
    /// Get the current AST node.
    BaseAST* currentNode() const;
    
    /// @name Convenience Queries
    bool insideFunction() const;
    bool insideLoop() const;
    bool insideSwitch() const;
    
    /// @name Current Node Getters
    FuncDeclAST* currentFunction() const;
    StmtAST* currentLoop() const;
    SwitchStmtAST* currentSwitch() const;
    BlockStmtAST* currentBlock() const;

    // ─── Return Tracking ──────────────────────────────────────────────────

    /// @brief Push an expected return type for the current function body.
    /// 
    /// Called when entering a function body. The type will be used by
    /// resolveReturnStmt to validate return values.
    /// 
    /// @param returnType The expected return type.
    void pushReturnType(const TypeAST* returnType) {
        m_returnStack.push(returnType);
    }
    
    /// @brief Pop the current return type.
    /// 
    /// Called when exiting a function body.
    void popReturnType() {
        m_returnStack.pop();
    }
    
    /// @brief Get the current expected return type.
    /// 
    /// This is used by resolveReturnStmt to validate return values.
    /// For curried functions, this returns the innermost expected type.
    /// 
    /// @return The current expected return type, or nullptr if none.
    const TypeAST* currentReturnType() const {
        return m_returnStack.current();
    }
    
    /// @brief Check if there are any pending return requirements.
    /// 
    /// @return true if there's at least one expected return type on the stack.
    bool hasReturnRequirements() const {
        return !m_returnStack.empty();
    }

    // ─── Type Narrowing ──────────────────────────────────────────────────

    /// @name If Condition Context
    /// 
    /// Used during if condition analysis to detect narrowing patterns.
    bool isIfConditionCtx() const;
    void setIfConditionCtx(bool isIfCtx);
    void setHasElse(bool hasElse);
    bool hasElse() const;
    
    /// @name Pending Narrowing
    /// 
    /// Narrowing info detected during condition analysis, to be applied
    /// to the appropriate branch.
    void setPendingNarrowing(const NarrowingInfo& info);
    const NarrowingInfo& getPendingNarrowing() const;
    void clearPendingNarrowing();
    
    /// @name Narrowing Stack
    /// 
    /// Active narrowing levels for branches and blocks.
    void pushNarrowingLevel(bool isInverse = false);
    void popNarrowingLevel();
    void narrowVariable(InternedString name, const TypeAST* type);
    const TypeAST* getNarrowedType(InternedString name) const;
    bool isNarrowingInverse() const;
    
    /// @name Pending Inverse Narrowing
    /// 
    /// For standalone if with early exit - applies to the rest of the block.
    void setPendingInverseNarrowing(const NarrowingInfo& info);
    bool hasPendingInverseNarrowing() const;
    const NarrowingInfo& getPendingInverseNarrowing() const;
    void clearPendingInverseNarrowing();

private:
    // ─── Members ──────────────────────────────────────────────────────────

    /// Main context stack.
    std::vector<ContextFrame> m_stack;
    
    /// Stack of expected return types for nested functions.
    /// 
    /// Each function body pushes its expected return type when entered
    /// and pops it when exited. Return statements validate against the
    /// top of this stack.
    ReturnStack m_returnStack;

    /// Type narrowing stack (separate because it can persist across contexts).
    struct NarrowingLevel {
        std::unordered_map<InternedString, const TypeAST*> narrowedTypes;
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