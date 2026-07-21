/**
 * @file SemanticContextStack.hpp
 * @brief Tracks the current semantic context for validation rules.
 *
 * Answers questions like:
 *   - Is `return` legal here?
 *   - Is `break` legal here?
 *   - Is `await` legal here?
 *
 * @architectural_note Independent of Scope stack
 *   A single Scope (e.g., a function body's block) may open exactly one
 *   SemanticContext frame (FuncBody), but nested blocks inside that same
 *   function push additional Scopes without pushing additional SemanticContext
 *   frames — `current()` still reports FuncBody for an `if` block nested
 *   inside a function.
 *
 *   A `for` loop nested in that function, by contrast, pushes both:
 *   a Scope (for the loop variable) AND a LoopBody frame.
 */
#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/SourceLocation.hpp"

#include <vector>

namespace sema {

/**
 * @brief The kind of semantic construct currently being analyzed.
 */
enum class SemanticContext {
    TopLevel,       ///< Module-level declarations (no function context)
    FuncBody,       ///< Inside a function body (return allowed)
    LoopBody,       ///< Inside a loop body (break/continue allowed)
    SwitchBody,     ///< Inside a switch body (case/default allowed)
    AsyncBody,      ///< Inside an async function (await allowed)
    GeneratorBody,  ///< Inside a generator function (yield allowed)
    ParallelBody,   ///< Inside a parallel/spawn block
};

/**
 * @brief Human-readable name for a SemanticContext.
 */
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

/**
 * @brief One frame of the semantic context stack.
 */
struct SemanticFrame {
    SemanticContext kind;     ///< The kind of context
    BaseAST* node;           ///< The AST node that opened this context
    SourceLocation openedAt; ///< Where the construct was opened
};

/**
 * @brief Semantic context stack manager.
 *
 * Tracks nested semantic contexts for validation rules.
 */
class SemanticContextStack {
public:
    // ─── Constructor ─────────────────────────────────────────────────────

    SemanticContextStack() = default;

    // ─── Push/Pop ────────────────────────────────────────────────────────

    /**
     * @brief Push a new semantic context frame.
     *
     * Prefer constructing a ScopedSemanticContext instead of calling this directly.
     */
    void push(SemanticContext kind, BaseAST* node, const SourceLocation& loc);

    /**
     * @brief Pop the innermost semantic context frame.
     *
     * Prefer letting a ScopedSemanticContext's destructor call this.
     */
    void pop();

    // ─── Queries ─────────────────────────────────────────────────────────

    /**
     * @brief Get the current (innermost) semantic context.
     *
     * Returns SemanticContext::TopLevel when the stack is empty.
     */
    SemanticContext current() const;

    /**
     * @brief Get the current context's AST node.
     */
    BaseAST* currentNode() const;

    /**
     * @brief True if `kind` is open anywhere on the stack.
     */
    bool isInside(SemanticContext kind) const;

    /**
     * @brief Current nesting depth.
     */
    size_t depth() const { return m_stack.size(); }

    /**
     * @brief Get the stack (for saving/restoring).
     */
    const std::vector<SemanticFrame>& stack() const { return m_stack; }

    /**
     * @brief Set the stack (for restoring).
     */
    void setStack(std::vector<SemanticFrame> stack) { m_stack = std::move(stack); }

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
};

} // namespace sema