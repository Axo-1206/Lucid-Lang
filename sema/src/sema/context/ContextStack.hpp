/// @file ContextStack.hpp
/// @brief Traversal-level state: where are we, and what is narrowed here?
///
/// ─── Role ─────────────────────────────────────────────────────────────────
/// `ContextStack` answers two questions during a recursive descent through
/// the AST:
///
///   1. **Where are we?** — inside a function? a sequence? a loop? an
///      if-condition? a block? The context stack tracks this as a stack
///      of `ContextFrame`s.
///   2. **What is narrowed here?** — a `T?` value that has been proven
///      non-nil by an enclosing `if x != nil` (or `x == nil` with an
///      early exit). The narrowing stack tracks this.
///
/// It emits **no diagnostics** and enforces **no rules**. It is a query
/// surface: call sites (`SemaStmt.cpp`, `SemaExpr.cpp`, `SequenceChecker`)
/// ask it what is true here, then decide what to do. This separation is
/// deliberate — the stack must be usable by any pass that needs flow
/// information, not just by a diagnostic-emitting one.
///
/// ─── What is not here (vs. the previous design) ───────────────────────────
/// No generics context, no async/spawn pending lists, no return stack.
/// The return stack is gone because a function body has exactly one
/// return type, read from the current frame's `expectedReturnType`. The
/// curried/nested return types that required a stack are not in the new
/// grammar.
///
/// ─── The three sub-stacks ─────────────────────────────────────────────────
///
/// ```
/// ┌──────────────────────────────────────────────────────────────────────┐
/// │                         ContextStack                                 │
/// │                                                                      │
/// │  ┌────────────────┐  ┌─────────────────┐                             │
/// │  │ Context stack  │  │ Narrowing stack │                             │
/// │  │ (where are we) │  │ (what's narrow) │                             │
/// │  ├────────────────┤  ├─────────────────┤                             │
/// │  │ SequenceBody   │  │ { x → int }     │                             │
/// │  │ LoopBody       │  │ { }             │                             │
/// │  │ IfStmt         │  │                 │                             │
/// │  │ Block          │  │                 │                             │
/// │  └────────────────┘  └─────────────────┘                             │
/// └──────────────────────────────────────────────────────────────────────┘
/// ```
///
/// ## Context stack
///
/// Tracks the syntactic construct for validation rules:
///   - `FuncBody`     — `return` is allowed; `wait*` is not.
///   - `SequenceBody` — `return` is allowed; `wait*` is allowed.
///   - `LoopBody`     — `break`/`continue` are allowed.
///   - `SwitchBody`   — `case`/`default` are allowed.
///   - `IfStmt`       — narrowing detection runs during condition analysis.
///   - `Block`        — pending inverse narrowing can be applied on entry.
///
/// ## Narrowing stack
///
/// Tracks flow-sensitive refinements of `T?` values:
///   - `if x != nil { ... }` narrows `x` to `T` inside the then-branch.
///   - `if x == nil { return }` narrows `x` to `T` in the rest of the block.
///
/// `&T` is not narrowed by the type system — it is already nilable, and
/// dereferencing a `nil` `&T` is a runtime panic (§5.2). A user who wants
/// to avoid the panic checks `!= nil` before dereferencing, but the check
/// does not narrow the type; it just guards a runtime operation.
///
/// ─── Mixed conditions ─────────────────────────────────────────────────────
/// A single condition may mix `!=` and `==` checks against `nil` only if
/// all checks use the same operator. `if x != nil and y != nil` is fine;
/// `if x != nil and y == nil` is rejected by the caller before a
/// `NarrowingInfo` is built. `NarrowingInfo::isEquality` is a single flag
/// for the whole condition, which is why mixing is not representable.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/SourceLocation.hpp"
#include "core/memory/InternedString.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lucid::sema {

// ─── ContextKind ──────────────────────────────────────────────────────────

/// @brief The kinds of syntactic context the stack tracks.
enum class ContextKind : uint8_t {
    TopLevel,      ///< Module level.
    FuncBody,      ///< Inside an ordinary `FN` body.
    SequenceBody,  ///< Inside a `@sequence` body (`wait*` allowed).
    LoopBody,      ///< Inside a loop body (`break`/`continue` allowed).
    SwitchBody,    ///< Inside a `switch` body (`case`/`default` allowed).
    IfStmt,        ///< Inside an if-statement (narrowing detection).
    Block,         ///< Inside a block (pending inverse narrowing).
};

// ─── NarrowingInfo ────────────────────────────────────────────────────────

/// @brief The narrowing effect of a condition.
///
/// A condition of the form `x != nil` narrows `x` to its non-nil type in
/// the then-branch. `x == nil` narrows `x` in the else-branch. The
/// `isEquality` flag records which operator the condition used, so the
/// resolver knows whether the narrowing applies directly or inversely.
///
/// `narrowings` maps a variable name to the type it takes once narrowed.
/// For `x: int?`, the entry is `{ x → int }`.
struct NarrowingInfo {
    bool hasNarrowing = false;
    std::unordered_map<InternedString, TypeAST*> narrowings;
    bool isEquality = false;
};

// ─── ContextFrame ─────────────────────────────────────────────────────────

/// @brief One frame on the context stack.
struct ContextFrame {
    ContextKind kind;
    BaseAST*    node = nullptr;

    // FuncBody / SequenceBody
    TypeAST* expectedReturnType = nullptr;

    // LoopBody / SwitchBody
    StmtAST*       loopStmt   = nullptr;
    SwitchStmtAST* switchStmt = nullptr;

    // IfStmt
    bool          isIfConditionCtx = false;
    bool          hasElse          = false;
    NarrowingInfo pendingNarrowing;

    // Block
    bool          hasPendingInverseNarrowing = false;
    NarrowingInfo pendingInverseNarrowing;
};

// ─── ContextStack ─────────────────────────────────────────────────────────

class ContextStack {
public:
    // ─── Push / pop ─────────────────────────────────────────────────────
    //
    // Prefer the RAII guards in SemaContext.hpp (`ScopedContext`,
    // `ScopedFunction`, etc.) over calling push/pop directly. The raw
    // push/pop pair exists so the guards have something to call.

    void push(ContextKind kind, BaseAST* node);

    /// Push a function frame. `kind` must be `FuncBody` or
    /// `SequenceBody`; the caller (`ScopedFunction`) chooses it from
    /// `FnDeclAST::isSequence`. A null `returnType` means the function
    /// returns `unit`.
    void pushFunction(FnDeclAST* decl, ContextKind kind, TypeAST* returnType);

    void pop();

    // ─── Context queries ────────────────────────────────────────────────

    ContextKind current() const;
    bool        isInside(ContextKind kind) const;
    BaseAST*    currentNode() const;

    /// True inside an ordinary function body or a sequence body.
    bool insideFunction() const;

    /// True inside a `@sequence` body only. This is the check the
    /// sequence-restriction rules use to allow `wait*` statements.
    bool insideSequence() const;

    bool insideLoop()   const;
    bool insideSwitch() const;

    StmtAST*       currentLoop()   const;
    SwitchStmtAST* currentSwitch() const;
    BlockStmtAST*  currentBlock()  const;

    /// The expected return type of the innermost function or sequence
    /// body. Null means `unit`.
    TypeAST* currentReturnType() const;

    // ─── If-condition context ───────────────────────────────────────────
    //
    // Set by `ScopedIfCondition` for the duration of condition analysis.
    // Narrowing detection reads it to know when a `==`/`!=` against
    // `nil` is a narrowing site rather than an ordinary comparison.

    bool isIfConditionCtx() const;
    void setIfConditionCtx(bool value);
    void setHasElse(bool value);
    bool hasElse() const;

    // ─── Pending narrowing ──────────────────────────────────────────────
    //
    // Set during condition analysis, read by the branch resolvers. Cleared
    // by `ScopedIfCondition`'s constructor.

    void setPendingNarrowing(const NarrowingInfo& info);
    const NarrowingInfo& getPendingNarrowing() const;
    void clearPendingNarrowing();

    // ─── Narrowing stack ────────────────────────────────────────────────

    void     pushNarrowingLevel(bool isInverse = false);
    void     popNarrowingLevel();
    void     narrowVariable(InternedString name, TypeAST* type);
    TypeAST* getNarrowedType(InternedString name) const;
    bool     isNarrowingInverse() const;

    // ─── Pending inverse narrowing ──────────────────────────────────────
    //
    // For a standalone `if x == nil { return }`, the inverse narrowing
    // (`x` is non-nil after the `if`) is stored on the enclosing block
    // frame and applied when the block resumes.

    void setPendingInverseNarrowing(const NarrowingInfo& info);
    bool hasPendingInverseNarrowing() const;
    const NarrowingInfo& getPendingInverseNarrowing() const;
    void clearPendingInverseNarrowing();

private:
    // ─── Members ────────────────────────────────────────────────────────
    std::vector<ContextFrame> m_stack;

    struct NarrowingLevel {
        std::unordered_map<InternedString, TypeAST*> narrowedTypes;
        bool isInverse = false;
    };
    std::vector<NarrowingLevel> m_narrowing;

    // ─── Frame search helpers ───────────────────────────────────────────

    ContextFrame*       findInnermostIfContext();
    const ContextFrame* findInnermostIfContext() const;
    ContextFrame*       findInnermostBlock();
    const ContextFrame* findInnermostBlock() const;
};

} // namespace lucid::sema