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
/// | Does the current function need returns? | `hasReturnRequirements()`       |
/// | Are all returns satisfied?              | `returnRequirementsSatisfied()` |
/// | Is a type being defined?                | `isDefiningType(T)`             |
///
/// ## Small Program Example
///
/// Consider this Lucid program:
/// ```lucid
/// package example
///
/// struct Node<T> {                    // [decl1] struct definition
///     value T,                        // [field1] field with generic param
///     next *Node<T>?                  // [field2] self-referential field
/// }
///
/// const process (n Node<int>) -> int { // [decl2] function definition
///     if n != nil {                    // [stmt1] if statement
///         let x int = n.value + 1      // [stmt2] variable declaration
///         return x                     // [stmt3] return statement
///     } else {
///         return 0                     // [stmt4] return statement
///     }
/// }
/// ```
///
/// ## Context Stack Evolution
///
/// ```
/// Step 1: Entering module
/// ┌─────────────────────────────────────────────┐
/// │ Stack: []                                   │
/// │ current() = TopLevel                        │
/// └─────────────────────────────────────────────┘
///
/// Step 2: Resolving struct Node<T> (registerStructName)
/// ┌─────────────────────────────────────────────┐
/// │ Stack: [                                    │
/// │   { kind: TopLevel, node: module }          │
/// │ ]                                           │
/// │ current() = TopLevel                        │
/// │ definingTypes = [Node]  ← self-ref enabled  │
/// └─────────────────────────────────────────────┘
///
/// Step 3: Resolving field 'next '*Node<T>?'
/// ┌─────────────────────────────────────────────┐
/// │ Stack: [                                    │
/// │   { kind: TopLevel, node: module }          │
/// │ ]                                           │
/// │ definingTypes = [Node]                      │
/// │ isDefiningType(Node) → true ✅              │
/// │ → This is a self-reference, allowed via ptr │
/// └─────────────────────────────────────────────┘
///
/// Step 4: Entering function process
/// ┌─────────────────────────────────────────────┐
/// │ Stack: [                                    │
/// │   { kind: TopLevel, node: module },         │
/// │   { kind: FuncBody, node: process,          │
/// │     returnGroups: [{ returnType: int,       │
/// │                      requiresReturn: true,  │
/// │                      level: 0,              │
/// │                      isSatisfied: false }]  │
/// │   }                                         │
/// │ ]                                           │
/// │ current() = FuncBody                        │
/// │ hasReturnRequirements() → true              │
/// └─────────────────────────────────────────────┘
///
/// Step 5: Analyzing if condition 'n != nil'
/// ┌─────────────────────────────────────────────┐
/// │ Stack: [                                    │
/// │   { kind: TopLevel, node: module },         │
/// │   { kind: FuncBody, node: process, ... },   │
/// │   { kind: IfStmt, node: stmt1,              │
/// │     isIfConditionCtx: true,                 │
/// │     hasElse: true,                          │
/// │     pendingNarrowing: { hasNarrowing: true, │
/// │                         narrowings: {n→int}}│
/// │   }                                         │
/// │ ]                                           │
/// │ isIfConditionCtx() → true                   │
/// │ getPendingNarrowing() → {n→int}             │
/// └─────────────────────────────────────────────┘
///
/// Step 6: Entering then branch with narrowing
/// ┌─────────────────────────────────────────────┐
/// │ Stack: [                                    │
/// │   { kind: TopLevel },                       │
/// │   { kind: FuncBody, ... },                  │
/// │   { kind: IfStmt, ... },                    │
/// │   { kind: Block, node: thenBranch }         │
/// │ ]                                           │
/// │ NarrowingStack: [                           │
/// │   Level 0: { n→int, isInverse: false }      │
/// │ ]                                           │
/// │ getNarrowedType(n) → int ✅                 │
/// └─────────────────────────────────────────────┘
///
/// Step 7: Processing return statement
/// ┌─────────────────────────────────────────────┐
/// │ Stack: [                                    │
/// │   { kind: TopLevel },                       │
/// │   { kind: FuncBody, node: process,          │
/// │     returnGroups: [{ returnType: int,       │
/// │                      requiresReturn: true,  │
/// │                      level: 0,              │
/// │                      isSatisfied: true }]   │
/// │   },                                        │
/// │   { kind: IfStmt, ... },                    │
/// │   { kind: Block, ... }                      │
/// │ ]                                           │
/// │ returnRequirementsSatisfied() → true ✅     │
/// └─────────────────────────────────────────────┘
/// ```
///
/// ## Node Storage Hierarchy
///
/// ```
/// ModuleAST (root)
///   └── decls: [decl1, decl2, ...]
///         ├── StructDeclAST (decl1)
///         │   └── fields: [field1, field2, ...]
///         │         ├── FieldDeclAST (field1): type = NamedTypeAST("T")
///         │         └── FieldDeclAST (field2): type = NullableTypeAST(
///         │                                       PtrTypeAST(
///         │                                         NamedTypeAST("Node<T>")
///         │                                       )
///         │                                     )
///         └── FuncDeclAST (decl2)
///             └── body: BlockStmtAST
///                   └── stmts: [stmt1, stmt2, stmt3, stmt4]
///                         ├── IfStmtAST (stmt1)
///                         │   ├── condition: BinaryExprAST(n != nil)
///                         │   ├── thenBranch: BlockStmtAST
///                         │   │   └── stmts: [stmt2, stmt3]
///                         │   └── elseBranch: BlockStmtAST
///                         │       └── stmts: [stmt4]
///                         ├── DeclStmtAST (stmt2)
///                         │   └── decl: VarDeclAST(x)
///                         ├── ReturnStmtAST (stmt3)
///                         │   └── value: BinaryExprAST(n.value + 1)
///                         └── ReturnStmtAST (stmt4)
///                             └── value: LiteralExprAST(0)
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
    AsyncBody,     ///< Inside an async function (await allowed)
    ParallelBody,  ///< Inside a parallel/spawn block
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
    
    /// Operator type for ALL narrowings in this struct.
    /// - true: All narrowings come from == comparisons
    /// - false: All narrowings come from != comparisons
    bool isEquality = false;
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
    
    // ─── Return Groups (only for FuncBody) ─────────────────────────────
    
    /// @brief One return group in a curried function.
    /// 
    /// Each `->` in the function signature creates one group.
    /// Example: `(a int) -> (b int) -> int` has 2 groups.
    struct ReturnGroup {
        const TypeAST* returnType = nullptr;  ///< Expected return type
        bool requiresReturn = false;          ///< True if this group needs a return
        bool isSatisfied = false;             ///< Has this group been satisfied?
        int level = 0;                        ///< Nesting level for this group
    };
    
    std::vector<ReturnGroup> returnGroups;    ///< All return groups in order
    int currentGroupIndex = -1;               ///< Currently active group
    int currentLevel = 0;                     ///< Current nesting level
    
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
    
    /// Push a function context with return tracking.
    void pushFunction(FuncDeclAST* node, FuncTypeAST* funcType, const SourceLocation& loc);
    
    /// Push a loop context.
    void pushLoop(StmtAST* loopStmt, const SourceLocation& loc);
    
    /// Push a switch context.
    void pushSwitch(SwitchStmtAST* switchStmt, const SourceLocation& loc);
    
    /// Push a block context.
    void pushBlock(BlockStmtAST* block, const SourceLocation& loc);
    
    /// Pop the innermost context.
    void pop();

    // ─── Queries ──────────────────────────────────────────────────────────

    /// Get the current context kind.
    ContextKind current() const;
    
    /// Check if we're inside a specific context kind.
    bool isInside(ContextKind kind) const;
    
    /// Get the current AST node.
    BaseAST* currentNode() const;
    
    /// @name Convenience Queries
    /// @{
    bool insideFunction() const;
    bool insideLoop() const;
    bool insideSwitch() const;
    bool insideAsync() const;
    bool insideParallel() const;
    /// @}
    
    /// @name Current Node Getters
    /// @{
    FuncDeclAST* currentFunction() const;
    StmtAST* currentLoop() const;
    SwitchStmtAST* currentSwitch() const;
    BlockStmtAST* currentBlock() const;
    /// @}

    // ─── Type Narrowing ──────────────────────────────────────────────────

    /// @name If Condition Context
    /// 
    /// Used during if condition analysis to detect narrowing patterns.
    /// @{
    bool isIfConditionCtx() const;
    void setIfConditionCtx(bool isIfCtx);
    void setHasElse(bool hasElse);
    bool hasElse() const;
    /// @}
    
    /// @name Pending Narrowing
    /// 
    /// Narrowing info detected during condition analysis, to be applied
    /// to the appropriate branch.
    /// @{
    void setPendingNarrowing(const NarrowingInfo& info);
    const NarrowingInfo& getPendingNarrowing() const;
    void clearPendingNarrowing();
    /// @}
    
    /// @name Narrowing Stack
    /// 
    /// Active narrowing levels for branches and blocks.
    /// @{
    void pushNarrowingLevel(bool isInverse = false);
    void popNarrowingLevel();
    void narrowVariable(InternedString name, const TypeAST* type);
    const TypeAST* getNarrowedType(InternedString name) const;
    bool isNarrowingInverse() const;
    /// @}
    
    /// @name Pending Inverse Narrowing
    /// 
    /// For standalone if with early exit - applies to the rest of the block.
    /// @{
    void setPendingInverseNarrowing(const NarrowingInfo& info);
    bool hasPendingInverseNarrowing() const;
    const NarrowingInfo& getPendingInverseNarrowing() const;
    void clearPendingInverseNarrowing();
    /// @}

    // ─── Return Requirements ─────────────────────────────────────────────

    /// @name Return Tracking
    /// 
    /// For curried functions, tracks which `->` groups have been satisfied.
    /// @{
    bool hasReturnRequirements() const;
    bool returnRequirementsSatisfied() const;
    void advanceReturnGroup();
    const ContextFrame::ReturnGroup* currentReturnGroup() const;
    void enterLevel();
    void exitLevel();
    /// @}

private:
    // ─── Members ──────────────────────────────────────────────────────────

    /// Main context stack.
    std::vector<ContextFrame> m_stack;

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
    
    void buildReturnGroups(FuncTypeAST* funcType, 
                           std::vector<ContextFrame::ReturnGroup>& groups);
};

} // namespace sema