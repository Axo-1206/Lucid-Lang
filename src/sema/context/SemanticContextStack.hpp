/// @file SemanticContextStack.hpp
/// @brief Tracks the current semantic context for validation rules.
/// 
/// Answers questions like:
///   - Is `return` legal here?
///   - Is `break` legal here?
///   - Is `await` legal here?
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

namespace sema {

/// @brief The kind of semantic construct currently being analyzed.
enum class SemanticContext {
    TopLevel,       ///< Module-level declarations (no function context)
    FuncBody,       ///< Inside a function body (return allowed)
    LoopBody,       ///< Inside a loop body (break/continue allowed)
    SwitchBody,     ///< Inside a switch body (case/default allowed)
    AsyncBody,      ///< Inside an async function (await allowed)
    GeneratorBody,  ///< Inside a generator function (yield allowed)
    ParallelBody,   ///< Inside a parallel/spawn block
};

/// @brief Human-readable name for a SemanticContext.
inline const char* semanticContextName(SemanticContext kind) {
    switch (kind) {
        case SemanticContext::TopLevel:      return "top level";
        case SemanticContext::FuncBody:      return "function body";
        case SemanticContext::LoopBody:      return "loop body";
        case SemanticContext::SwitchBody:    return "switch body";
        case SemanticContext::AsyncBody:     return "async body";
        case SemanticContext::GeneratorBody: return "generator body";
        case SemanticContext::ParallelBody:  return "parallel body";
    }
    return "unknown context";
}

/// @brief Represents the return requirements for a function context.
struct ReturnRequirements {
    /// @brief One return group in a curry chain.
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
    
    // ─── Level Management ────────────────────────────────────────────────
    
    /// @brief Enter a new nesting level.
    void enterLevel() { currentLevel++; }
    
    /// @brief Exit the current nesting level.
    void exitLevel() { 
        if (currentLevel > 0) currentLevel--; 
    }
    
    /// @brief Get the current nesting level.
    int getCurrentLevel() const { return currentLevel; }
    
    // ─── Group Management ─────────────────────────────────────────────────
    
    /// @brief Check if there are any pending groups at the current level.
    bool hasPendingGroupAtCurrentLevel() const {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel) {
                return true;
            }
        }
        return false;
    }
    
    /// @brief Get the next pending group at the current level.
    const Group* getNextPendingGroupAtCurrentLevel() const {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                return &groups[i];
            }
        }
        return nullptr;
    }
    
    /// @brief Get the current group (the first unsatisfied group at current level).
    const Group* currentGroup() const {
        for (int i = currentGroupIndex + 1; i < (int)groups.size(); ++i) {
            if (groups[i].requiresReturn && groups[i].level == currentLevel && !groups[i].isSatisfied) {
                return &groups[i];
            }
        }
        return nullptr;
    }
    
    /// @brief Mark the current group as satisfied and advance.
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
    bool isSatisfied() const {
        for (const auto& g : groups) {
            if (g.requiresReturn && !g.isSatisfied) return false;
        }
        return true;
    }
    
    /// @brief Check if there are any requirements.
    bool hasRequirements() const {
        for (const auto& g : groups) {
            if (g.requiresReturn) return true;
        }
        return false;
    }
};

/// @brief One frame of the semantic context stack.
struct SemanticFrame {
    SemanticContext kind;          ///< The kind of context
    BaseAST* node;                 ///< The AST node that opened this context
    SourceLocation openedAt;       ///< Where the construct was opened
    
    // ─── Function Requirements (only valid for FuncBody contexts) ──────
    ReturnRequirements returnReqs;
    
    // ─── Loop/Switch Tracking ────────────────────────────────────────────
    StmtAST* loopStmt = nullptr;   ///< The loop statement (for LoopBody)
    SwitchStmtAST* switchStmt = nullptr; ///< The switch statement (for SwitchBody)
};

/// @brief Semantic context stack manager.
/// Tracks nested semantic contexts for validation rules.
class SemanticContextStack {
public:
    // ─── Constructor ─────────────────────────────────────────────────────

    SemanticContextStack() = default;

    // ─── Push/Pop ────────────────────────────────────────────────────────

    /// @brief Push a new semantic context frame.
    void push(SemanticContext kind, BaseAST* node, const SourceLocation& loc);

    /// @brief Push a function body context with return requirements.
    void pushFunction(FuncDeclAST* node, FuncTypeAST* funcType, const SourceLocation& loc);

    /// @brief Push an anonymous function body context with return requirements.
    void pushAnonFunction(AnonFuncExprAST* node, FuncTypeAST* funcType, const SourceLocation& loc);

    /// @brief Push a loop body context.
    void pushLoop(StmtAST* loopStmt, const SourceLocation& loc);

    /// @brief Push a switch body context.
    void pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc);

    /// @brief Pop the innermost semantic context frame.
    void pop();

    // ─── Queries ─────────────────────────────────────────────────────────

    /// @brief Get the current (innermost) semantic context.
    SemanticContext current() const;

    /// @brief Get the current context's AST node.
    BaseAST* currentNode() const;

    /// @brief True if `kind` is open anywhere on the stack.
    bool isInside(SemanticContext kind) const;

    /// @brief Current nesting depth.
    size_t depth() const { return m_stack.size(); }

    /// @brief Get the stack (for saving/restoring).
    const std::vector<SemanticFrame>& stack() const { return m_stack; }

    /// @brief Set the stack (for restoring).
    void setStack(std::vector<SemanticFrame> stack) { m_stack = std::move(stack); }

    // ─── Return Requirements Queries ──────────────────────────────────

    /// @brief Get the current return requirements (if inside a function).
    const ReturnRequirements* currentReturnReqs() const;

    /// @brief Get the current return requirements (mutable).
    ReturnRequirements* currentReturnReqsMutable();

    /// @brief True if we're inside a function that has return requirements.
    bool hasReturnRequirements() const;

    /// @brief Check if all return requirements are satisfied.
    bool returnRequirementsSatisfied() const;

    /// @brief Advance to the next return group at the current level.
    void advanceReturnGroup();

    /// @brief Get the current return group (nullptr if none).
    const ReturnRequirements::Group* currentReturnGroup() const;

    /// @brief Check if there's a pending requirement at the current level.
    bool hasPendingRequirementAtCurrentLevel() const;

    /// @brief Enter a new nesting level.
    void enterLevel();

    /// @brief Exit the current nesting level.
    void exitLevel();

    /// @brief Get the current nesting level.
    int getCurrentLevel() const;

    // ─── Convenience Queries ─────────────────────────────────────────────

    /// True if we're currently inside a function body (of any flavor).
    bool insideFunction() const;

    /// True if we're currently inside a loop body.
    bool insideLoop() const;

    /// True if we're currently inside a switch body.
    bool insideSwitch() const;

    /// True if we're currently inside an async context.
    bool insideAsync() const;

    /// True if we're currently inside a generator context.
    bool insideGenerator() const;

    /// True if we're currently inside a parallel/spawn context.
    bool insideParallel() const;

    /// Get the innermost function declaration (if any).
    FuncDeclAST* currentFunction() const;

    /// Get the innermost loop statement (if any).
    StmtAST* currentLoop() const;

    /// Get the innermost switch statement (if any).
    SwitchStmtAST* currentSwitch() const;

private:
    // ─── Members ─────────────────────────────────────────────────────────

    std::vector<SemanticFrame> m_stack;

    // ─── Helpers ─────────────────────────────────────────────────────────

    /// @brief Build return requirements from a function type.
    ReturnRequirements buildReturnRequirements(FuncTypeAST* funcType);

    /// @brief Find the innermost function frame (if any).
    SemanticFrame* findInnermostFunction();

    const SemanticFrame* findInnermostFunction() const;
};

} // namespace sema