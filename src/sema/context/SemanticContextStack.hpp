/// @file SemanticContextStack.hpp
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
///   SemanticContext frame (FuncBody), but nested blocks inside that same
///   function push additional Scopes without pushing additional SemanticContext
///   frames — `current()` still reports FuncBody for an `if` block nested
///   inside a function.
/// 
///   A `for` loop nested in that function, by contrast, pushes both:
///   a Scope (for the loop variable) AND a LoopBody frame.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/SourceLocation.hpp"

#include <vector>
#include <optional>
#include <unordered_map>

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// SemanticContext - What kind of construct we're inside
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The kind of semantic construct currently being analyzed.
/// 
/// Each frame on the context stack has one of these kinds. The kind determines
/// what statements are legal (e.g., `return` is only legal inside FuncBody,
/// `break` is only legal inside LoopBody or SwitchBody).
enum class SemanticContext {
    TopLevel,       ///< Module-level declarations (no function context)
    FuncBody,       ///< Inside a function body (return allowed)
    LoopBody,       ///< Inside a loop body (break/continue allowed)
    SwitchBody,     ///< Inside a switch body (case/default allowed)
    AsyncBody,      ///< Inside an async function (await allowed)
    GeneratorBody,  ///< Inside a generator function (yield allowed)
    ParallelBody,   ///< Inside a parallel/spawn block
    IfStmt,         ///< Inside an if statement (for type narrowing)
    Block,          ///< Inside a block statement (for pending inverse narrowing)
};

/// @brief Human-readable name for a SemanticContext.
/// Used for diagnostic messages.
inline const char* semanticContextName(SemanticContext kind) {
    switch (kind) {
        case SemanticContext::TopLevel:      return "top level";
        case SemanticContext::FuncBody:      return "function body";
        case SemanticContext::LoopBody:      return "loop body";
        case SemanticContext::SwitchBody:    return "switch body";
        case SemanticContext::AsyncBody:     return "async body";
        case SemanticContext::GeneratorBody: return "generator body";
        case SemanticContext::ParallelBody:  return "parallel body";
        case SemanticContext::IfStmt:        return "if statement";
        case SemanticContext::Block:         return "block";
    }
    return "unknown context";
}

// ─────────────────────────────────────────────────────────────────────────────
// ReturnRequirements - Tracking curried function return requirements
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents the return requirements for a function context.
/// 
/// For a curried function like `(a int) -> (b int) -> int`, there are two
/// return groups: group 0 requires returning a function, group 1 requires
/// returning an int. Each group is tracked separately with its own level.
/// 
/// The `currentGroupIndex` tracks which group is currently being analyzed.
/// Each `return` statement advances to the next group.
struct ReturnRequirements {
    /// ─────────────────────────────────────────────────────────────────────────────
    /// RETURN GROUPS — How Curried Function Returns Are Tracked
    /// ─────────────────────────────────────────────────────────────────────────────
    ///
    /// For a curried function like `(a int) -> (b int) -> int`, each `->` creates
    /// a "return group" that the body must satisfy. The groups are tracked in order:
    ///
    ///   const add (a int) -> (b int) -> int = {
    ///       // Group 0: (a int) -> (b int) -> int  → requires returning a function
    ///       // Group 1: (b int) -> int             → requires returning an int
    ///       
    ///       return (b int) -> int {
    ///           // ─── Group 0 satisfied here ───
    ///           // The `return (b int) -> int { ... }` returns a function 
    ///           // that takes (b int) -> int
    ///           
    ///           return a + b
    ///           // ─── Group 1 satisfied here ───
    ///           // The inner `return a + b` returns the final int
    ///       }
    ///   }
    ///
    /// ─── What is a "Pending Group"? ────────────────────────────────────────────
    ///
    /// A "pending group" is a return group that hasn't been satisfied yet.
    /// 
    /// When we enter a function body, groups are pending in order. Each `return`
    /// statement satisfies the current pending group and advances to the next.
    ///
    /// Example:
    ///   const f ()() -> () -> int = {
    ///       // Groups: [Group 0: (no arrow), Group 1: (arrow → func), Group 2: (arrow → int)]
    ///       // Initially: Group 0 is pending
    ///       // Group 0 is automatically satisfied (no arrow, no return needed)
    ///       
    ///       return () -> int {  // ← Satisfies Group 1 (returns a function)
    ///           return 42  // ← Satisfies Group 2 (returns int)
    ///       }
    ///   }
    ///
    /// ─── Level Matching — Why `level` Matters ──────────────────────────────────
    ///
    /// Each group has a `level` that matches the nesting depth of its corresponding
    /// `return` statement. This ensures that a `return` at the wrong depth doesn't
    /// accidentally satisfy the wrong group.
    ///
    /// Example:
    ///   const f () -> () -> int = {
    ///       // Level 0: Group 0 requires returning a function
    ///       
    ///       return 42  // ❌ WRONG LEVEL: Level 0, but this is a value, not a function
    ///       
    ///       return {   // ✅ CORRECT: Level 0 returns a function
    ///           // Level 1: Group 1 requires returning int
    ///           return 42  // ✅ CORRECT: Level 1 returns int
    ///       }
    ///   }
    ///
    /// ─── Group States ──────────────────────────────────────────────────────────
    ///
    /// Each group has three possible states:
    ///   1. Pending     — Not yet satisfied, and the current level is at this group
    ///   2. Satisfied   — A `return` statement at the correct level satisfied it
    ///   3. Not Required — `requiresReturn == false` (no arrow, no return needed)
    ///
    /// ─── Why We Need These Functions ──────────────────────────────────────────
    ///
    /// The functions below work together to track which group is currently pending
    /// and whether it needs to be satisfied at the current nesting level:
    ///
    ///   hasPendingGroupAtCurrentLevel()
    ///     → Check if there's a group that needs a `return` right now.
    ///     Example: In `() -> () -> int`, at level 0 there is a pending group
    ///     (must return a function), so this returns true.
    ///
    ///   getNextPendingGroupAtCurrentLevel()
    ///     → Get the specific pending group that needs a `return` at this level.
    ///     Used to check what type the `return` must produce.
    ///
    ///   currentGroup()
    ///     → Get the current unsatisfied group that matches this level.
    ///     Used by analyzeReturnStmt to validate the return value's type.
    ///
    ///   advanceGroup()
    ///     → Mark the current group as satisfied and move to the next.
    ///     Called after a valid `return` statement is found.
    ///
    ///   isSatisfied()
    ///     → Check if ALL groups have been satisfied (or are not required).
    ///     Called when exiting a function body to ensure no missing returns.
    ///
    /// ─── Example: Tracking Groups Through a Function ──────────────────────────
    ///
    ///   const process (a int)(b int) -> (c int) -> int = {
    ///       // Groups:
    ///       //   [0] (a int)(b int) → (c int) → int  → requiresReturn=false (no arrow)
    ///       //   [1] (c int) → int                   → requiresReturn=true  (arrow → func)
    ///       //   [2] int                             → requiresReturn=true  (arrow → int)
    ///       
    ///       // ─── Level 0 ──────────────────────────────────────────────────────
    ///       // hasPendingGroupAtCurrentLevel() → false (Group 0 doesn't require return)
    ///       // currentGroup() → nullptr (no pending group at this level)
    ///       
    ///       return {  
    ///           // ─── Level 1 ──────────────────────────────────────────────────
    ///           // hasPendingGroupAtCurrentLevel() → true (Group 1 requires return)
    ///           // currentGroup() → Group 1 (isCurried=true, returnType=func)
    ///           // advanceGroup() → Group 1 satisfied, currentGroupIndex = 1
    ///           
    ///           return 42
    ///           // ─── Level 2 ──────────────────────────────────────────────────
    ///           // hasPendingGroupAtCurrentLevel() → true (Group 2 requires return)
    ///           // currentGroup() → Group 2 (isCurried=false, returnType=int)
    ///           // advanceGroup() → Group 2 satisfied, currentGroupIndex = 2
    ///       }
    ///       
    ///       // ─── After all groups satisfied ──────────────────────────────────
    ///       // isSatisfied() → true (all groups satisfied)
    ///   }
    ///
    /// ─── Summary ──────────────────────────────────────────────────────────────
    ///
    /// The group tracking system ensures that:
    ///   1. Each `->` in a function signature creates a return requirement
    ///   2. `return` statements satisfy requirements in order
    ///   3. The nesting level (`level`) ensures returns are at the correct depth
    ///   4. All requirements must be satisfied before the function body exits
    ///   5. Groups without `->` (hasArrow=false) never require a return
    struct Group {
        const TypeAST* returnType = nullptr;   // Expected return type (nullptr for void)
        bool isCurried = false;                // True if must return a function
        bool requiresReturn = false;           // True if this group requires a return
        bool isSatisfied = false;              // Has this group been satisfied?
        int level = 0;                         // Which nesting level this group belongs to
        SourceLocation satisfiedAt;            // Where it was satisfied (for diagnostics)
    };
    
    std::vector<Group> groups;                 // All return groups in order
    int currentGroupIndex = -1;                // Currently active group (-1 = none)
    int currentLevel = 0;                      // Current nesting level
    bool isVoid = false;                       // No return required (codegen wraps)
    bool allowsOptionalReturn = false;         // `return;` is allowed
    
    /// @brief Enter a new nesting level (called when entering a block).
    void enterLevel() { currentLevel++; }
    
    /// @brief Exit the current nesting level (called when exiting a block).
    void exitLevel() { if (currentLevel > 0) currentLevel--; }
    
    /// @brief Get the current nesting level.
    int getCurrentLevel() const { return currentLevel; }
    
    /// @brief Check if there are any pending groups at the current level.
    /// Used to determine if a return is required at the current nesting level.
    bool hasPendingGroupAtCurrentLevel() const {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel) {
                return true;
            }
        }
        return false;
    }
    
    /// @brief Get the next pending group at the current level.
    /// Returns nullptr if no pending group at this level.
    const Group* getNextPendingGroupAtCurrentLevel() const {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                return &groups[i];
            }
        }
        return nullptr;
    }
    
    /// @brief Get the current group (the first unsatisfied group at current level).
    /// Used by analyzeReturnStmt to validate the return value against the expected type.
    const Group* currentGroup() const {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                return &groups[i];
            }
        }
        return nullptr;
    }
    
    /// @brief Mark the current group as satisfied and advance.
    /// Called by analyzeReturnStmt after validating a return statement.
    void advanceGroup() {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                groups[i].isSatisfied = true;
                currentGroupIndex = i;
                return;
            }
        }
    }
    
    /// @brief Check if all requirements are satisfied.
    /// Called by analyzeBlock when exiting a function body to ensure all returns are present.
    bool isSatisfied() const {
        for (const auto& g : groups) {
            if (g.requiresReturn && !g.isSatisfied) return false;
        }
        return true;
    }
    
    /// @brief Check if there are any requirements.
    /// Used to determine if return checking is needed.
    bool hasRequirements() const {
        for (const auto& g : groups) {
            if (g.requiresReturn) return true;
        }
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// NarrowingInfo - Type narrowing information from if conditions
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Information about a type narrowing from an if condition.
/// 
/// Supports multiple variables (for `or` at top level):
///   if a == nil or b == nil { return }
///   -- inverse: a != nil AND b != nil
///   -- both a and b are narrowed
/// 
/// The `isEquality` flag distinguishes between `==` and `!=`:
///   - `==` (isEquality = true):  variable is nil/err in then branch
///   - `!=` (isEquality = false): variable is non-nullable/non-fallible in then branch
struct NarrowingInfo {
    bool hasNarrowing = false;
    std::unordered_map<InternedString, const TypeAST*> narrowings;  // varName → narrowed type
    bool isEquality = false;  // true for ==, false for !=
};

// ─────────────────────────────────────────────────────────────────────────────
// TypeNarrowingStack - Tracks narrowed types per scope level
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One level of type narrowing for variables.
/// Each level corresponds to a branch (then, else) or inverse narrowing.
struct NarrowingLevel {
    std::unordered_map<InternedString, const TypeAST*> narrowedTypes;
    bool isInverse = false;  // Inverse narrowing applies (for rest of scope)
};

/// @brief Stack of type narrowing contexts.
/// 
/// When entering a branch (then/else), we push a level and apply narrowings.
/// When exiting the branch, we pop the level.
/// 
/// The stack is searched from innermost to outermost when looking up a variable.
class TypeNarrowingStack {
public:
    /// @brief Push a new narrowing level.
    /// @param isInverse True if this is inverse narrowing (applies to rest of scope).
    void pushLevel(bool isInverse = false);
    
    /// @brief Pop the current narrowing level.
    void popLevel();
    
    /// @brief Narrow a variable in the current level.
    /// @param name The variable name.
    /// @param type The narrowed type (e.g., int instead of int?).
    void narrow(InternedString name, const TypeAST* type);
    
    /// @brief Get the narrowed type for a variable.
    /// Searches from innermost to outermost. Returns nullptr if not narrowed.
    const TypeAST* getNarrowedType(InternedString name) const;
    
    /// @brief Check if current narrowing level is inverse.
    bool isInverse() const;
    
    /// @brief Check if there's any active narrowing.
    bool hasNarrowing() const;
    
    /// @brief Clear all narrowing levels (for testing/cleanup).
    void clear();
    
private:
    std::vector<NarrowingLevel> m_stack;
};

// ─────────────────────────────────────────────────────────────────────────────
// SemanticFrame - One frame on the context stack
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One frame of the semantic context stack.
/// 
/// Each frame represents a semantic construct (function, loop, if, block, etc.)
/// and stores context-specific information needed for validation.
struct SemanticFrame {
    SemanticContext kind;          ///< The kind of context
    BaseAST* node;                 ///< The AST node that opened this context
    SourceLocation openedAt;       ///< Where the construct was opened
    
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
// SemanticContextStack - Main context stack manager
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Semantic context stack manager.
/// 
/// Tracks nested semantic contexts for validation rules. Provides:
///   - Push/pop for different context kinds (function, loop, switch, if, block)
///   - Queries for current context (isInside, currentFunction, etc.)
///   - Return requirement tracking for curried functions
///   - Type narrowing for if conditions
///   - Pending inverse narrowing for standalone if with early exit
class SemanticContextStack {
public:
    // ─── Constructor ─────────────────────────────────────────────────────

    /// @brief Default constructor. Initializes empty stack.
    SemanticContextStack();

    // ─── Push/Pop ────────────────────────────────────────────────────────

    /// @brief Push a new semantic context frame.
    /// Generic push for any context kind. For function contexts, use pushFunction/pushAnonFunction instead.
    void push(SemanticContext kind, BaseAST* node, const SourceLocation& loc);

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
    /// Returns SemanticContext::TopLevel when the stack is empty.
    SemanticContext current() const;

    /// @brief Get the current context's AST node.
    /// Returns nullptr if the stack is empty.
    BaseAST* currentNode() const;

    /// @brief True if `kind` is open anywhere on the stack.
    /// Used to check if we're inside a function, loop, switch, etc.
    bool isInside(SemanticContext kind) const;

    /// @brief Current nesting depth.
    size_t depth() const { return m_stack.size(); }

    /// @brief Get the stack (for saving/restoring).
    const std::vector<SemanticFrame>& stack() const { return m_stack; }

    /// @brief Set the stack (for restoring).
    void setStack(std::vector<SemanticFrame> stack) { m_stack = std::move(stack); }

    // ─── Return Requirements Queries ──────────────────────────────────

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

    std::vector<SemanticFrame> m_stack;  ///< The actual context stack
    TypeNarrowingStack m_narrowing;      ///< Type narrowing stack (separate from context)

    // ─── Helpers ─────────────────────────────────────────────────────────

    /// @brief Build return requirements from a function type.
    /// Walks the curry chain and creates a ReturnRequirements for each group.
    ReturnRequirements buildReturnRequirements(FuncTypeAST* funcType);

    /// @brief Find the innermost function frame (if any).
    SemanticFrame* findInnermostFunction();
    const SemanticFrame* findInnermostFunction() const;

    /// @brief Find the innermost if context frame (if any).
    SemanticFrame* findInnermostIfContext();
    const SemanticFrame* findInnermostIfContext() const;

    /// @brief Find the innermost block frame (if any).
    SemanticFrame* findInnermostBlock();
    const SemanticFrame* findInnermostBlock() const;
};

} // namespace sema