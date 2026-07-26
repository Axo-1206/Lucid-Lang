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