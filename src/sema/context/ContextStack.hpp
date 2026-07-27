/// @file ContextStack.hpp
/// @brief Tracks the current semantic context for validation rules.
/// 
/// Answers questions like:
///   - Is `return` legal here?
///   - Is `break` legal here?
///   - Is `await` legal here?
///   - Are we in an if condition (for type narrowing)?
/// 
/// @architectural_note Independent of Scope stack
///   A single Scope (e.g., a function body's block) may open exactly one
///   ContextKind frame (FuncBody), but nested blocks inside that same
///   function push additional Scopes without pushing additional ContextKind
///   frames — `current()` still reports FuncBody for an `if` block nested
///   inside a function.
/// 
///   A `for` loop nested in that function, by contrast, pushes both:
///   a Scope (for the loop variable) AND a LoopBody frame.

#pragma once

#include "ContextKind.hpp"
#include "ReturnRequirements.hpp"
#include "NarrowingStack.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/SourceLocation.hpp"

#include <vector>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// ContextFrame - One frame on the context stack
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One frame of the semantic context stack.
/// 
/// Each frame represents a semantic construct (function, loop, if, block, etc.)
/// and stores context-specific information needed for validation.
struct ContextFrame {
    ContextKind kind;          ///< The kind of context
    BaseAST* node;             ///< The AST node that opened this context
    SourceLocation openedAt;   ///< Where the construct was opened
    
    // ─── Function Requirements (only valid for FuncBody contexts) ──────
    ReturnRequirements returnReqs;  ///< Return requirements for curried functions
    
    // ─── Loop/Switch Tracking ────────────────────────────────────────────
    StmtAST* loopStmt = nullptr;         ///< The loop statement (for LoopBody)
    SwitchStmtAST* switchStmt = nullptr; ///< The switch statement (for SwitchBody)
    
    // ─── If/Else Narrowing ───────────────────────────────────────────────
    bool isIfConditionCtx = false;      ///< Currently analyzing an if condition
    bool hasElse = false;               ///< If statement has an else branch
    NarrowingInfo pendingNarrowing;     ///< Narrowing info from the condition (supports multiple vars)
    
    // ─── Pending Inverse Narrowing ──────────────────────────────────────
    /// When a standalone if with early exit is encountered, the inverse
    /// narrowing is applied to the rest of the current block.
    /// This is tracked at the block level.
    bool hasPendingInverseNarrowing = false;
    NarrowingInfo pendingInverseNarrowing;  ///< Supports multiple vars
};

// ─────────────────────────────────────────────────────────────────────────────
// ContextStack - Main context stack manager
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Semantic context stack manager.
/// 
/// Tracks nested semantic contexts for validation rules. Provides:
///   - Push/pop for different context kinds (function, loop, switch, if, block)
///   - Queries for current context (isInside, currentFunction, etc.)
///   - Return requirement tracking for curried functions (delegated to ReturnRequirements)
///   - Type narrowing for if conditions (delegated to NarrowingStack)
///   - Pending inverse narrowing for standalone if with early exit
class ContextStack {
public:
    // ─── Constructor ─────────────────────────────────────────────────────

    /// @brief Default constructor. Initializes empty stack.
    ContextStack() = default;

    // ─── Push/Pop ────────────────────────────────────────────────────────

    /// @brief Push a new semantic context frame.
    /// Generic push for any context kind. For function contexts, use pushFunction/pushAnonFunction instead.
    void push(ContextKind kind, BaseAST* node, const SourceLocation& loc);

    /// @brief Push a function body context with return requirements.
    /// Called by analyzeFuncDecl. Builds ReturnRequirements from the function type.
    void pushFunction(FuncDeclAST* node, FuncTypeAST* funcType, const SourceLocation& loc);

    /// @brief Push an anonymous function body context with return requirements.
    /// Called by checkAnonFuncExpr. Builds ReturnRequirements from the function type.
    void pushAnonFunction(AnonFuncExprAST* node, FuncTypeAST* funcType, const SourceLocation& loc);

    /// @brief Push a loop body context.
    /// Called by analyzeForStmt, analyzeWhileStmt, analyzeDoWhileStmt.
    void pushLoop(StmtAST* loopStmt, const SourceLocation& loc);

    /// @brief Push a switch body context.
    /// Called by analyzeSwitchStmt.
    void pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc);

    /// @brief Push a block context.
    /// Called by analyzeBlock. Enables pending inverse narrowing tracking.
    void pushBlock(BlockStmtAST* block, const SourceLocation& loc);

    /// @brief Pop the innermost semantic context frame.
    /// Called when exiting any context (function, loop, switch, if, block).
    void pop();

    // ─── Queries ─────────────────────────────────────────────────────────

    /// @brief Get the current (innermost) semantic context.
    /// Returns ContextKind::TopLevel when the stack is empty.
    ContextKind current() const;

    /// @brief Get the current context's AST node.
    /// Returns nullptr if the stack is empty.
    BaseAST* currentNode() const;

    /// @brief True if `kind` is open anywhere on the stack.
    /// Used to check if we're inside a function, loop, switch, etc.
    bool isInside(ContextKind kind) const;

    /// @brief Current nesting depth.
    size_t depth() const { return m_stack.size(); }

    /// @brief Get the stack (for saving/restoring).
    const std::vector<ContextFrame>& stack() const { return m_stack; }

    /// @brief Set the stack (for restoring).
    void setStack(std::vector<ContextFrame> stack) { m_stack = std::move(stack); }

    // ─── Return Requirements Queries ────────────────────────────────────

    /// @brief Get the current return requirements (if inside a function).
    /// Returns nullptr if not inside a function.
    const ReturnRequirements* currentReturnReqs() const;

    /// @brief Get the current return requirements (mutable).
    /// Used by analyzeReturnStmt to advance the current group.
    ReturnRequirements* currentReturnReqsMutable();

    /// @brief True if we're inside a function that has return requirements.
    bool hasReturnRequirements() const;

    /// @brief Check if all return requirements are satisfied.
    /// Called by analyzeBlock when exiting a function body.
    bool returnRequirementsSatisfied() const;

    /// @brief Advance to the next return group at the current level.
    /// Called by analyzeReturnStmt after validating a return.
    void advanceReturnGroup();

    /// @brief Get the current return group (nullptr if none).
    /// Used by analyzeReturnStmt to validate return values against expected type.
    const ReturnRequirements::Group* currentReturnGroup() const;

    /// @brief Check if there's a pending requirement at the current level.
    bool hasPendingRequirementAtCurrentLevel() const;

    /// @brief Enter a new nesting level.
    /// Called when entering a block inside a curried function.
    void enterLevel();

    /// @brief Exit the current nesting level.
    /// Called when exiting a block inside a curried function.
    void exitLevel();

    /// @brief Get the current nesting level.
    int getCurrentLevel() const;

    // ─── If Condition Context ─────────────────────────────────────────

    /// @brief Set whether we're currently analyzing an if condition.
    /// Called by analyzeIfStmt before analyzing the condition.
    void setIfConditionCtx(bool isIfCtx);

    /// @brief Check if we're currently analyzing an if condition.
    /// Used by checkBinaryExpr to detect type narrowing patterns.
    bool isIfConditionCtx() const;

    /// @brief Set whether the current if has an else branch.
    /// Used to determine if inverse narrowing should apply.
    void setHasElse(bool hasElse);

    /// @brief Check if the current if has an else branch.
    bool hasElse() const;

    /// @brief Set the pending narrowing info from the condition.
    /// Called by checkBinaryExpr when it detects a narrowing pattern.
    void setPendingNarrowing(const NarrowingInfo& info);

    /// @brief Get the pending narrowing info.
    /// Used by analyzeIfStmt to apply narrowing to branches.
    const NarrowingInfo& getPendingNarrowing() const;

    /// @brief Clear the pending narrowing info.
    void clearPendingNarrowing();

    // ─── Type Narrowing ──────────────────────────────────────────────

    /// @brief Push a new narrowing level.
    /// Called when entering a branch (then/else) with narrowing.
    void pushNarrowingLevel(bool isInverse = false);

    /// @brief Pop the current narrowing level.
    /// Called when exiting a branch.
    void popNarrowingLevel();

    /// @brief Narrow a variable in the current level.
    /// Called by analyzeIfStmt to apply narrowing to variables.
    void narrowVariable(InternedString name, const TypeAST* type);

    /// @brief Get the narrowed type for a variable.
    /// Used by checkIdentifierExpr to use the narrowed type.
    const TypeAST* getNarrowedType(InternedString name) const;

    /// @brief Check if current narrowing level is inverse.
    bool isNarrowingInverse() const;

    /// @brief Check if there's any active narrowing.
    bool hasActiveNarrowing() const;

    // ─── Pending Inverse Narrowing ──────────────────────────────────

    /// @brief Set pending inverse narrowing for the current block.
    /// Called by analyzeIfStmt for standalone if with early exit.
    void setPendingInverseNarrowing(const NarrowingInfo& info);

    /// @brief Check if there's pending inverse narrowing in the current block.
    bool hasPendingInverseNarrowing() const;

    /// @brief Get the pending inverse narrowing info.
    /// Used by analyzeBlock to apply inverse narrowing to the rest of the block.
    const NarrowingInfo& getPendingInverseNarrowing() const;

    /// @brief Clear pending inverse narrowing in the current block.
    /// Called by analyzeBlock after applying inverse narrowing.
    void clearPendingInverseNarrowing();

    // ─── Convenience Queries ─────────────────────────────────────────────

    /// @brief True if we're currently inside a function body (of any flavor).
    bool insideFunction() const;

    /// @brief True if we're currently inside a loop body.
    bool insideLoop() const;

    /// @brief True if we're currently inside a switch body.
    bool insideSwitch() const;

    /// @brief True if we're currently inside an async context.
    bool insideAsync() const;

    /// @brief True if we're currently inside a generator context.
    bool insideGenerator() const;

    /// @brief True if we're currently inside a parallel/spawn context.
    bool insideParallel() const;

    /// @brief Get the innermost function declaration (if any).
    FuncDeclAST* currentFunction() const;

    /// @brief Get the innermost loop statement (if any).
    StmtAST* currentLoop() const;

    /// @brief Get the innermost switch statement (if any).
    SwitchStmtAST* currentSwitch() const;

    /// @brief Get the innermost block statement (if any).
    BlockStmtAST* currentBlock() const;

private:
    // ─── Members ─────────────────────────────────────────────────────────

    std::vector<ContextFrame> m_stack;  ///< The actual context stack

    // ─────────────────────────────────────────────────────────────────────
    // TYPE NARROWING FLOW — Complete Lifecycle
    // ─────────────────────────────────────────────────────────────────────
    //
    // The `ContextStack` manages two separate but related stacks:
    //
    //   1. ContextFrame Stack (m_stack): Tracks semantic contexts (function, loop, if, block)
    //   2. NarrowingStack (m_narrowing): Tracks type narrowing per scope level
    //
    // These stacks work together to implement type narrowing.
    //
    // ─── Case 1: Then Branch (Direct Narrowing) ────────────────────────
    //
    //   Source: if x != nil { println(x) }
    //
    //   ┌─────────────────────────────────────────────────────────────────────┐
    //   │ Step 1: analyzeIfStmt()                                             │
    //   │   └── push(ContextKind::IfStmt)                                     │
    //   │   └── setIfConditionCtx(true)                                       │
    //   │       └── checkExpr(condition)                                      │
    //   │           └── checkBinaryExpr() detects x != nil                    │
    //   │               └── setPendingNarrowing({x → int, isEquality=false})  │
    //   │   └── setIfConditionCtx(false)                                      │
    //   │   └── info = getPendingNarrowing()                                  │
    //   │   └── clearPendingNarrowing()                                       │
    //   ├─────────────────────────────────────────────────────────────────────┤
    //   │ Step 2: analyzeBlock(thenBranch)                                    │
    //   │   └── pushBlock()                                                   │
    //   │   └── pushNarrowingLevel(false)    ← isInverse=false                │
    //   │   └── narrowVariable(x, int)                                        │
    //   │   └── analyze statements...    ← x is int here                      │
    //   │   └── popNarrowingLevel()          ← x returns to int?              │
    //   │   └── pop()                                                         │
    //   └─────────────────────────────────────────────────────────────────────┘
    //
    // ─── Case 2: Else Branch (Inverse Narrowing) ──────────────────────
    //
    //   Source: if x != nil { ... } else { println(x) }  // x is nil here
    //
    //   ┌─────────────────────────────────────────────────────────────────┐
    //   │ Step 1: Same as Case 1 for condition analysis                   │
    //   ├─────────────────────────────────────────────────────────────────┤
    //   │ Step 2: analyzeBlock(elseBranch)                                │
    //   │   └── pushBlock()                                               │
    //   │   └── pushNarrowingLevel(true)     ← isInverse=true             │
    //   │   └── narrowVariable(x, int)       ← x is non-nullable          │
    //   │   └── analyze statements...    ← x is int here (inverse)        │
    //   │   └── popNarrowingLevel()                                       │
    //   │   └── pop()                                                     │
    //   └─────────────────────────────────────────────────────────────────┘
    //
    // ─── Case 3: Standalone If with Early Return (Inverse Narrowing) ──
    //
    //   Source: 
    //     if x == nil { return }
    //     println(x)  // x is int here (inverse narrowing)
    //
    //   ┌─────────────────────────────────────────────────────────────────────┐
    //   │ Step 1: analyzeIfStmt()                                             │
    //   │   └── push(ContextKind::IfStmt)                                     │
    //   │   └── setIfConditionCtx(true)                                       │
    //   │       └── checkExpr(condition)                                      │
    //   │           └── checkBinaryExpr() detects x == nil                    │
    //   │               └── setPendingNarrowing({x → int, isEquality=true}    │
    //   │   └── setIfConditionCtx(false)                                      │
    //   │   └── info = getPendingNarrowing()                                  │
    //   │   └── clearPendingNarrowing()                                       │
    //   ├─────────────────────────────────────────────────────────────────────┤
    //   │ Step 2: analyzeBlock(thenBranch)                                    │
    //   │   └── pushBlock()                                                   │
    //   │   └── pushNarrowingLevel(false)                                     │
    //   │   └── For equality (isEquality=true): NO narrowing applied          │
    //   │       (x is nil, not a definite type)                               │
    //   │   └── analyze statements...    ← x is nil here (not useful)         │
    //   │   └── popNarrowingLevel()                                           │
    //   │   └── pop()                                                         │
    //   ├─────────────────────────────────────────────────────────────────────┤
    //   │ Step 3: Check for early return                                      │
    //   │   └── if (!hasElse && thenReturns && hasNarrowing && isEquality) {  │
    //   │       └── setPendingInverseNarrowing(info)  ← CRITICAL!             │
    //   │   }                                                                 │
    //   ├─────────────────────────────────────────────────────────────────────┤
    //   │ Step 4: analyzeBlock(parentBlock) continues...                      │
    //   │   └── pushBlock(parentBlock)                                        │
    //   │   └── hasPendingInverseNarrowing() → true                           │
    //   │   └── pushNarrowingLevel(true)    ← isInverse=true                  │
    //   │   └── narrowVariable(x, int)      ← x is non-nullable               │
    //   │   └── println(x)    ← x is int here                                 │
    //   │   └── popNarrowingLevel()                                           │
    //   │   └── clearPendingInverseNarrowing()                                │
    //   │   └── pop()                                                         │
    //   └─────────────────────────────────────────────────────────────────────┘
    //
    // ─── Why We Need Pending Inverse Narrowing ──────────────────────────
    //
    // The early return pattern requires inverse narrowing that applies to the
    // REST OF THE BLOCK, not just the else branch. We can't apply it immediately
    // because:
    //   1. We don't know if the then branch actually returns until we analyze it
    //   2. The parent block's scope hasn't been entered yet when we're in the if
    //   3. We need to apply the narrowing at the block level, not the if level
    //
    // The `pendingInverseNarrowing` field in ContextFrame acts as a "deferred
    // narrowing" that gets applied when analyzeBlock sees it.
    //
    // ─── Conditions for Pending Inverse Narrowing ───────────────────────
    //
    //   ✓ Standalone if (no else branch)
    //   ✓ Then branch transfers control (returns, breaks, or continues)
    //   ✓ Has narrowing info from condition
    //   ✓ Condition uses equality (==) - not inequality (!=)
    //     - x == nil → inverse: x != nil (narrows to T)
    //     - x == err → inverse: x != err (narrows to T)
    //     - x != nil → inverse: x == nil (narrows to nil, not useful)
    //     - x != err → inverse: x == err (narrows to err, not useful)
    //
    // ─── Example: `or` at Top Level ─────────────────────────────────────
    //
    //   Source: if a == nil or b == nil { return }
    //           println(a)  // a is int
    //           println(b)  // b is string
    //
    //   info = {a → int, b → string, isEquality=true}
    //   pendingInverseNarrowing = {a → int, b → string, isEquality=true}
    //   Both variables are narrowed in the parent block ✅
    //
    // ─── Example: `and` at Top Level ─────────────────────────────────────
    //
    //   Source: if a == nil and b == nil { return }
    //           println(a)  // a is still int? (not narrowed)
    //           println(b)  // b is still string? (not narrowed)
    //
    //   info = {} (empty, because 'and' is unsound)
    //   pendingInverseNarrowing = {} (no narrowing)
    //
    // ─── Example: Loop Context ──────────────────────────────────────────
    //
    //   Source: for _, item int? in items {
    //               if item == nil { continue }
    //               println(item)  // item is int here
    //           }
    //
    //   The `continue` acts as an early exit from the loop iteration.
    //   `pendingInverseNarrowing` works the same way - the narrowing applies
    //   to the rest of the block after the if.
    //
    // ─── Summary ──────────────────────────────────────────────────────────
    //
    //   ┌──────────────────┬──────────────────┬────────────────────────────┐
    //   │ Case             │ isInverse Flag   │ How Applied                │
    //   ├──────────────────┼──────────────────┼────────────────────────────┤
    //   │ Then Branch      │ false            │ Direct: pushNarrowingLevel │
    //   │ (x != nil)       │                  │ (false)                    │
    //   ├──────────────────┼──────────────────┼────────────────────────────┤
    //   │ Else Branch      │ true             │ Direct: pushNarrowingLevel │
    //   │ (inverse)        │                  │ (true)                     │
    //   ├──────────────────┼──────────────────┼────────────────────────────┤
    //   │ Standalone If    │ true             │ Deferred: setPending       │
    //   │ (early return)   │                  │ InverseNarrowing()         │
    //   └──────────────────┴──────────────────┴────────────────────────────┘
    //
    // @see analyzeIfStmt() in SemaStmt.cpp for the implementation
    // @see analyzeBlock() in SemaStmt.cpp for pending inverse narrowing application
    // @see TypeNarrowHelpers.hpp for narrowing extraction
    NarrowingStack m_narrowing;         ///< Type narrowing stack (separate from context)

    // ─── Helpers ─────────────────────────────────────────────────────────

    /// @brief Build return requirements from a function type.
    /// Walks the curry chain and creates a ReturnRequirements for each group.
    ReturnRequirements buildReturnRequirements(FuncTypeAST* funcType);

    /// @brief Find the innermost function frame (if any).
    ContextFrame* findInnermostFunction();
    const ContextFrame* findInnermostFunction() const;

    /// @brief Find the innermost if context frame (if any).
    ContextFrame* findInnermostIfContext();
    const ContextFrame* findInnermostIfContext() const;

    /// @brief Find the innermost block frame (if any).
    ContextFrame* findInnermostBlock();
    const ContextFrame* findInnermostBlock() const;
};

} // namespace sema