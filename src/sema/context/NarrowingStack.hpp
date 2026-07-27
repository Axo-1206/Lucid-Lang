/// @file NarrowingStack.hpp
/// @brief Tracks type narrowing per scope level for if-statement type refinement.
/// 
/// # Type Narrowing Overview
/// 
/// Type narrowing is a core feature of the language that allows the compiler to
/// refine the type of a variable based on conditional checks. For example:
/// 
/// ```lucid
/// let x int? = getOptionalValue()
/// if x != nil {
///     -- x is narrowed to `int` here (non-nullable)
///     let y int = x + 1  // ✅ x is known to be non-nil
/// }
/// -- x is back to `int?` here (outside the narrowing scope)
/// ```
/// 
/// # Stack Structure
/// 
/// The NarrowingStack maintains a stack of `NarrowingLevel` frames, where each
/// frame corresponds to a specific branch scope (then/else) or inverse narrowing
/// scope. The stack is searched from innermost to outermost when looking up a
/// narrowed type for a variable.
/// 
/// ```
/// ┌───────────────────────────────────────────────────────────────────┐
/// │                     NarrowingStack Structure                      │
/// ├───────────────────────────────────────────────────────────────────┤
/// │                                                                   │
/// │  ┌────────────────────────────────────────────────────────────┐   │
/// │  │  Level 2 (innermost) - Branch Level                        │   │
/// │  │  ┌─────────────────────────────────────────────────────┐   │   │
/// │  │  │  if x != nil { -- Then branch                       │   │   │
/// │  │  │      -- x narrowed to int                           │   │   │
/// │  │  │      -- level: {x → int}, isInverse: false          │   │   │
/// │  │  │  }                                                  │   │   │
/// │  │  └─────────────────────────────────────────────────────┘   │   │
/// │  ├────────────────────────────────────────────────────────────┤   │
/// │  │  Level 1 - Block Level                                     │   │
/// │  │  ┌─────────────────────────────────────────────────────┐   │   │
/// │  │  │  {  -- Block scope                                  │   │   │
/// │  │  │      if x == nil { return; } -- Early exit          │   │   │
/// │  │  │      -- x is inversely narrowed to int              │   │   │
/// │  │  │      -- level: {x → int}, isInverse: true           │   │   │
/// │  │  │  }                                                  │   │   │
/// │  │  └─────────────────────────────────────────────────────┘   │   │
/// │  ├────────────────────────────────────────────────────────────┤   │
/// │  │  Level 0 (outermost) - Function/Module Level               │   │
/// │  │  ┌─────────────────────────────────────────────────────┐   │   │
/// │  │  │  let x int? = getValue()                            │   │   │
/// │  │  │  -- No narrowing, x remains int?                    │   │   │
/// │  │  └─────────────────────────────────────────────────────┘   │   │
/// │  └────────────────────────────────────────────────────────────┘   │
/// └───────────────────────────────────────────────────────────────────┘
/// ```
/// 
/// # Types of Narrowing
/// 
/// ## 1. Direct Narrowing (Then Branch)
/// 
/// Occurs when a condition tests for a specific property of a variable:
/// 
/// ```lucid
/// if x != nil { -- Direct narrowing: x → int (non-nullable)
///     -- x is int
/// }
/// ```
/// 
/// The narrowing is stored in the `then` branch's level with `isInverse = false`.
/// 
/// ## 2. Inverse Narrowing (Else Branch / Early Exit)
/// 
/// Occurs in two scenarios:
/// 
/// ### A: Else Branch
/// ```lucid
/// if x != nil {
///     -- x is int (direct narrowing)
/// } else {
///     -- x is nil (inverse narrowing: x → nil)
/// }
/// ```
/// 
/// ### B: Early Exit (Standalone if)
/// ```lucid
/// if x == nil {
///     return; -- Early exit
/// }
/// // x is inversely narrowed to int (non-nullable) for the rest of the scope
/// ```
/// 
/// Inverse narrowing is stored with `isInverse = true` and applies to the
/// current block level (not a separate branch level).
/// 
/// ## 3. Pending Inverse Narrowing (Block Level)
/// 
/// For standalone if statements with early exit, the inverse narrowing is
/// "pending" at the block level until the block is fully analyzed:
/// 
/// ```lucid
/// {
///     if x == nil { return; } -- ← inverse narrowing pending
///     -- ── Pending inverse narrowing applied here ──
///     use(x)  -- x is int
/// }
/// ```
/// 
/// The `pendingInverseNarrowing` in `ContextFrame` stores this information
/// until it's applied to the rest of the block.
/// 
/// # Stack Search Rules
/// 
/// When looking up a narrowed type for a variable:
/// 
/// 1. **Search from innermost to outermost** (most specific to least specific)
/// 2. **Inverse narrowing overrides direct narrowing** at the same level
/// 3. **Outer narrowings don't affect inner scopes** (they're independent)
/// 4. **When a scope exits, its narrowing is discarded**
/// 
/// Example of nested narrowing:
/// 
/// ```lucid
/// let x int? = getValue()
/// if x != nil {                              // Level 1: x → int
///     if x > 0 {                             // Level 2: x → int (same, no change)
///         -- x is int
///     }
///     if x == 5 {                            // Level 2: x → int(5) (literal narrowing)
///         -- x is int(5)
///     }
/// }
/// ```
/// 
/// # Interaction with ContextStack
/// 
/// The `NarrowingStack` is a sub-component of `ContextStack` and works together
/// with it:
/// 
/// ```
/// ┌─────────────────────────────────────────────────────────────────────┐
/// │                        ContextStack                                 │
/// ├─────────────────────────────────────────────────────────────────────┤
/// │  ┌──────────────────────────────────────────────────────────────┐   │
/// │  │  ContextFrame Stack (SemanticContext)                        │   │
/// │  │  ┌───────────────────────────────────────────────────────┐   │   │
/// │  │  │  Frame: IfStmt (pendingNarrowing)                     │   │   │
/// │  │  │  └── Stores condition's narrowing info (pre-analysis) │   │   │
/// │  │  ├───────────────────────────────────────────────────────┤   │   │
/// │  │  │  Frame: Block (pendingInverseNarrowing)               │   │   │
/// │  │  │  └── Stores inverse narrowing for block-level         │   │   │
/// │  │  ├───────────────────────────────────────────────────────┤   │   │
/// │  │  │  Frame: FuncBody (returnReqs)                         │   │   │
/// │  │  └───────────────────────────────────────────────────────┘   │   │
/// │  ├──────────────────────────────────────────────────────────────┤   │
/// │  │  NarrowingStack (TypeNarrowing)                              │   │
/// │  │  ┌───────────────────────────────────────────────────────┐   │   │
/// │  │  │  Level 2: {x → int} (then branch)                     │   │   │
/// │  │  │  Level 1: {x → int, isInverse: true} (block level)    │   │   │
/// │  │  │  Level 0: {} (no narrowing)                           │   │   │
/// │  │  └───────────────────────────────────────────────────────┘   │   │
/// │  └──────────────────────────────────────────────────────────────┘   │
/// └─────────────────────────────────────────────────────────────────────┘
/// ```
/// 
/// # Example: Complete Narrowing Flow
/// 
/// ```lucid
/// // Source code:
/// let x int? = getValue()
/// let y string? = getString()
/// 
/// if x != nil and y != nil {
///     -- x int, y string
///     io:print(x)
///     io:print(y)
/// }
/// 
/// -- Narrowing stack after condition analysis:
/// -- Level 1 (then branch):
/// --   {x → int, y → string}
/// --   isInverse: false
/// -- 
/// -- When exiting the branch, Level 1 is popped.
/// ```
/// 
/// # Memory Management
/// 
/// - `NarrowingStack` stores **const pointers** to `TypeAST` nodes
/// - AST nodes are immutable and owned by the parser's arena
/// - No ownership transfer: the stack only references existing nodes
/// - Stack levels are automatically cleaned up when popped
/// 
/// @architectural_note Search Order
///   The search from innermost to outermost ensures that the most recent
///   narrowing takes precedence. This is correct because:
///   1. Inner scopes have more specific knowledge
///   2. Inverse narrowing in an inner scope should override outer narrowings
///   3. When a scope exits, its narrowings are removed from consideration
/// 
/// @architectural_note Separation from ContextStack
///   NarrowingStack is separate from ContextFrame stack because:
///   1. Narrowing levels correspond to branches, not contexts
///   2. Multiple narrowings can occur within the same context
///   3. Narrowing can persist across different context kinds
///   4. This separation follows the Single Responsibility Principle

#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"

#include <unordered_map>
#include <vector>

namespace sema {

/// @brief Information about a type narrowing from an if condition.
/// 
/// # Supported Patterns
/// 
/// The narrowing system supports conditions that use a single operator type:
/// 
/// ```lucid
/// // ✅ Supported - all ==
/// if a == nil or b == nil { return }
/// // → a and b are narrowed to nil
/// 
/// // ✅ Supported - all !=  
/// if a != nil and b != nil {
///     // a and b are narrowed to their non-nullable types
/// }
/// 
/// // ❌ Rejected - mixed operators at the same logical level
/// if a == nil or b != nil { return }  // No type narrowing here
/// if a != nil and b == nil { return }  // No type narrowing here
/// ```
/// 
/// # Why Mixed Operators Are Rejected
/// 
/// Mixing `==` and `!=` in the same condition creates ambiguous semantics:
/// 
/// ```lucid
/// if a == nil or b != nil { return }
/// -- After this condition:
/// -- - If a == nil is true: a is nil, but b is unknown
/// -- - If b != nil is true: b is non-nil, but a is unknown
/// -- - If both are true: a is nil AND b is non-nil
/// -- The inverse narrowing makes no sense here.
/// ```
/// 
/// The `isEquality` flag tracks the operator type for ALL narrowings in the
/// condition. Mixed operators are detected during semantic analysis and
/// reported as errors.
/// 
/// # Supports Multiple Variables
/// 
/// For `or` at top level with early exit:
/// ```lucid
/// if a == nil or b == nil { return }
/// -- inverse: a != nil AND b != nil
/// -- both a and b are narrowed
/// ```
/// 
/// The `isEquality` flag distinguishes between `==` and `!=`:
///   - `==` (isEquality = true):  variable is nil/err in then branch
///   - `!=` (isEquality = false): variable is non-nullable/non-fallible in then branch
/// 
/// @note This is stored in ContextFrame.pendingNarrowing during condition
///       analysis and then applied to the appropriate branch.
/// 
/// @invariant All narrowings in `narrowings` map to the same operator type
///            as indicated by `isEquality`. Mixed operators are rejected
///            during semantic analysis.
struct NarrowingInfo {
    /// @brief Whether this struct contains valid narrowing info.
    bool hasNarrowing = false;
    
    /// @brief Map from variable name to its narrowed type.
    /// 
    /// Example: `x → int` (when x was `int?`)
    std::unordered_map<InternedString, const TypeAST*> narrowings;
    
    /// @brief The operator type for ALL narrowings in this struct.
    /// 
    /// - `true`  → All narrowings come from `==` comparisons
    ///             Example: `if a == nil or b == nil { return; }`
    ///             → both a and b are narrowed to nil
    /// 
    /// - `false` → All narrowings come from `!=` comparisons
    ///             Example: `if a != nil and b != nil { ... }`
    ///             → both a and b are narrowed to their non-nullable types
    /// 
    /// @invariant All entries in `narrowings` were produced by the same
    ///            operator type as this flag.
    bool isEquality = false;  // true for ==, false for !=
};

/// @brief One level of type narrowing for variables.
/// 
/// Each level corresponds to a scope where narrowing applies:
/// - A branch (then/else) with direct narrowing: `isInverse = false`
/// - A block with inverse narrowing: `isInverse = true`
/// 
/// The level is created when entering the scope and destroyed when exiting.
/// 
/// @note Multiple variables can be narrowed in a single level (e.g., `x != nil and y != nil`)
/// 
/// # What `isInverse` Means
/// 
/// The `isInverse` flag tells the compiler HOW this narrowing was derived:
/// 
/// - `isInverse = false` (Direct Narrowing):
///   The condition directly proved the variable has the narrowed type.
///   Example: `if x != nil { ... }` → x is int inside the block
/// 
/// - `isInverse = true` (Inverse Narrowing):
///   The condition proved the OPPOSITE would have caused an exit,
///   so the variable must have the narrowed type in the remaining scope.
///   Example: `if x == nil { return; }` → x is int after the if
/// 
/// Both types of narrowing are stored in the same `NarrowingLevel` struct.
/// The `isInverse` flag is what distinguishes them, not the level itself.
struct NarrowingLevel {
    /// Map from variable name to its narrowed type
    std::unordered_map<InternedString, const TypeAST*> narrowedTypes;
    
    /// Flag indicating if this is inverse narrowing
    /// - `false`: Direct narrowing (e.g., `if x != nil { x is int }`)
    /// - `true`:  Inverse narrowing (e.g., `if x == nil { return }` leaves x as int)
    bool isInverse = false;
};

/// @brief Stack of type narrowing contexts.
/// 
/// # When to Push/Pop
/// 
/// ```cpp
/// // When entering a then branch with narrowing:
/// ctx.contexts.pushNarrowingLevel(false);  // isInverse = false
/// // ... analyze the branch
/// ctx.contexts.popNarrowingLevel();
/// 
/// // When entering an else branch with inverse narrowing:
/// ctx.contexts.pushNarrowingLevel(true);   // isInverse = true
/// // ... analyze the else branch
/// ctx.contexts.popNarrowingLevel();
/// 
/// // For standalone if with early exit, inverse narrowing is applied
/// // to the current block level:
/// ctx.contexts.pushNarrowingLevel(true);
/// // ... analyze rest of block
/// ctx.contexts.popNarrowingLevel();
/// ```
/// 
/// # Example: Multiple Narrowings
/// 
/// ```lucid
/// let x int? = getValue()
/// let y string? = getString()
/// 
/// if x != nil and y != nil {
///     -- Level 1: {x → int, y → string}
///     -- Both variables narrowed in one level
///     if x > 0 {
///         -- Level 2: {x → int} (same type, no change)
///         -- Stack search: Level 2 first, then Level 1
///         -- x is still int (no change)
///     }
/// }
/// 
/// // After exiting: both levels popped
/// ```
/// 
/// # Example: Inverse Narrowing with Multiple Variables
/// 
/// ```lucid
/// let x int? = getValue()
/// let y string? = getString()
/// 
/// if x == nil or y == nil {
///     return
/// }
/// -- Level 1 (block level, isInverse = true):
/// --   {x → int, y → string}
/// -- Both variables are inversely narrowed for the rest of the block
/// ```
/// 
/// # Implementation Notes
/// 
/// 1. **Search Order**: Innermost to outermost
///    - Provides correct shadowing behavior
///    - Inner narrowings override outer narrowings
///    - When a variable is narrowed multiple times, the innermost wins
/// 
/// 2. **Level Independence**: Each level is independent
///    - Narrowings in one level don't affect other levels
///    - Levels are completely independent of context frames
///    - This allows multiple narrowings within the same function body
/// 
/// 3. **Memory Management**: All types are AST nodes owned by the arena
///    - No new memory is allocated for narrowings
///    - The stack only stores references to existing AST nodes
///    - No need for deep copying or memory management
class NarrowingStack {
public:
    /// @brief Push a new narrowing level.
    /// 
    /// Called when entering a scope where narrowing applies:
    /// - Then branch of an if statement: `isInverse = false`
    /// - Else branch of an if statement: `isInverse = true`
    /// - Block after an early exit: `isInverse = true`
    /// 
    /// @param isInverse True if this is inverse narrowing (applies to rest of scope).
    /// 
    /// @note The level is pushed before any variables are narrowed.
    void pushLevel(bool isInverse = false);
    
    /// @brief Pop the current narrowing level.
    /// 
    /// Called when exiting a scope where narrowing was applied.
    /// All narrowings in this level are discarded.
    /// 
    /// @pre The stack must not be empty.
    void popLevel();
    
    /// @brief Narrow a variable in the current level.
    /// 
    /// Adds or updates a narrowing for a variable in the innermost level.
    /// 
    /// @param name The variable name.
    /// @param type The narrowed type (e.g., int instead of int?).
    /// 
    /// @pre A level must exist on the stack (pushLevel must have been called).
    /// 
    /// @note Multiple variables can be narrowed in the same level.
    void narrow(InternedString name, const TypeAST* type);
    
    /// @brief Get the narrowed type for a variable.
    /// 
    /// Searches the stack from innermost to outermost and returns the first
    /// narrowing found for the variable.
    /// 
    /// @param name The variable name to look up.
    /// @return The narrowed type, or nullptr if the variable is not narrowed.
    /// 
    /// @note The search stops at the first level that has a narrowing for this variable.
    ///       This means innermost narrowings override outer narrowings.
    const TypeAST* getNarrowedType(InternedString name) const;
    
    /// @brief Check if current narrowing level is inverse.
    /// 
    /// @return True if the innermost level has `isInverse = true`.
    /// 
    /// @note Returns false if the stack is empty.
    bool isInverse() const;
    
    /// @brief Check if there's any active narrowing.
    /// 
    /// @return True if any level has at least one narrowed variable.
    bool hasNarrowing() const;
    
    /// @brief Clear all narrowing levels (for testing/cleanup).
    void clear();
    
private:
    /// @brief The actual stack of narrowing levels.
    /// 
    /// Index 0 is the outermost level (pushed first).
    /// The back of the vector is the innermost level (most recent).
    std::vector<NarrowingLevel> m_stack;
};

} // namespace sema