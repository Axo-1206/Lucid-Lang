/// @file SemaStmt.cpp
/// @brief Implements Sema.hpp's "STATEMENTS - Control flow analysis" section.
/// 
/// @architectural_note Control Flow Analysis
///   Each statement analyzer returns a boolean indicating whether the statement
///   guarantees control transfer out of the enclosing block (return, break,
///   continue, or a block whose last statement guarantees it).
/// 
/// @architectural_note Statement Structure
///   Statements are read-only AST nodes. We validate them and determine
///   control flow behavior without modifying the AST.
/// 
/// @architectural_note Error Recovery
///   Even if a statement has errors, we continue analysis to find more errors.
///   The return value should reflect the statement's control flow behavior
///   regardless of errors (if the statement is syntactically a return, it
///   still transfers control).

#include "../Sema.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// =============================================================================
// analyzeStmt - Dispatch
// =============================================================================

/// @brief Dispatch a statement to its specific analyze*Stmt() function.
///
/// @param stmt The statement to analyze.
/// @param ctx The semantic context.
/// @return true if this statement guarantees control transfer out of the block.
bool analyzeStmt(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:        return analyzeBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::IfStmt:           return analyzeIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:       return analyzeSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::SwitchCase:       return analyzeSwitchCase(stmt->as<SwitchCaseAST>(), ctx);
        case ASTKind::ForStmt:          return analyzeForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WhileStmt:        return analyzeWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::DoWhileStmt:      return analyzeDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
        case ASTKind::ReturnStmt:       return analyzeReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:        return analyzeBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:     return analyzeContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::ExprStmt:         return analyzeExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::DeclStmt:         return analyzeDeclStmt(stmt->as<DeclStmtAST>(), ctx);
        case ASTKind::AsyncExpr:        return analyzeAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
        case ASTKind::AwaitExpr:        return analyzeAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
        case ASTKind::SpawnExpr:        return analyzeSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
        case ASTKind::JoinExpr:         return analyzeJoinStmt(stmt->as<JoinStmtAST>(), ctx);
        default:
            // Unknown/error-recovery statement
            return false;
    }
}

// =============================================================================
// analyzeBlock
// =============================================================================

/// @brief Analyze a block statement.
///
/// A block is a sequence of statements in a new scope.
/// The block returns true if its last statement guarantees control transfer.
///
/// @param block The block statement.
/// @param ctx The semantic context.
/// @return true if the block guarantees control transfer out of the block.
///
/// ─── Control Flow Analysis ──────────────────────────────────────────────────
///
/// The function tracks whether any statement in the block transfers control
/// (return, break, continue). Once a transfer is detected:
///   1. The block is considered to transfer control
///   2. Subsequent statements are unreachable
///   3. The function should warn about unreachable code
///
/// ─── Example ──────────────────────────────────────────────────────────────
///
/// ```lucid
/// {
///     let x int = 5
///     return x      // ← transfers = true
///     let y int = 10 // ← UNREACHABLE - should warn
/// }
/// ```
///
/// @todo Add diagnostic for unreachable code after control transfer.
///       Currently, statements after a transfer are silently skipped.
///       We should emit a warning (DiagCode::W1001 - UnreachableCode)
///       when this occurs.
bool analyzeBlock(const BlockStmtAST* block, SemaContext& ctx) {
    // ─── 1. Push block context for pending inverse narrowing ────────────
    ctx.contexts.pushBlock(const_cast<BlockStmtAST*>(block), block->loc);

    bool transfers = false;
    bool hasAppliedPendingNarrowing = false;

    // ─── 2. Apply pending inverse narrowing from previous statements ────
    // If there's pending inverse narrowing from a standalone if with early exit,
    // apply it before analyzing the rest of the block
    if (ctx.contexts.hasPendingInverseNarrowing()) {
        const NarrowingInfo& pendingInfo = ctx.contexts.getPendingInverseNarrowing();
        if (pendingInfo.hasNarrowing) {
            ctx.contexts.pushNarrowingLevel(true);
            for (const auto& [varName, narrowedType] : pendingInfo.narrowings) {
                ctx.contexts.narrowVariable(varName, narrowedType);
            }
            ctx.contexts.clearPendingInverseNarrowing();
            hasAppliedPendingNarrowing = true;
        }
    }

    // ─── 3. Analyze each statement in the block ──────────────────────────
    for (const StmtAST* stmt : block->stmts) {
        // ── 3a. Check if previous statement transferred control ──────────
        // If the previous statement transferred control, this statement is
        // unreachable. We should warn about it.
        //
        // TODO: Add unreachable code warning.
        //       Once DiagCode::W1001 (UnreachableCode) is defined, emit:
        //         ctx.warning(stmt, DiagCode::W1001,
        //                     "unreachable statement after control transfer");
        //
        //       For now, we silently skip unreachable statements to avoid
        //       analyzing dead code.
        if (transfers) {
            // The previous statement transferred control.
            // This statement is unreachable - skip it.
            //
            // TODO: Emit warning for unreachable code.
            //       This is a common source of bugs and should be reported.
            //
            // Note: We still need to continue the loop to check for
            //       any potential nested transfers, but since control
            //       already transferred, we don't need to analyze
            //       this or subsequent statements for control flow.
            //       However, we might want to still analyze for
            //       validation (e.g., type checking) to catch errors,
            //       but that's a separate concern.
            continue;
        }

        // ── 3b. Analyze the statement ──────────────────────────────────────
        transfers = analyzeStmt(stmt, ctx);
        
        // If the statement transfers control, subsequent statements are unreachable
        if (transfers) {
            // This statement transfers control (return, break, continue).
            // The next iteration will detect unreachable code.
            //
            // TODO: If we want to emit a warning for unreachable code,
            //       we should track the position of the transfer statement
            //       so we can report "code after this point is unreachable".
            break;
        }
    }

    // ─── 4. Pop pending narrowing level if we applied one ───────────────
    if (hasAppliedPendingNarrowing) {
        ctx.contexts.popNarrowingLevel();
    }

    // ─── 5. Pop block context ─────────────────────────────────────────────
    ctx.contexts.pop();

    // ─── 6. Final Check: Return Requirements ─────────────────────────────
    // If we're inside a function that has requirements, check they're satisfied
    if (ctx.contexts.hasReturnRequirements() && !ctx.contexts.returnRequirementsSatisfied()) {
        // Only report error if the block doesn't transfer control via return
        // If it transfers via break/continue, that's fine (loop/switch handles it)
        if (!transfers) {
            ctx.error(block, DiagCode::E3005,
                      "function is missing a return statement");
        }
    }

    return transfers;
}


// =============================================================================
// analyzeIfStmt
// =============================================================================

/// @brief Analyze an if statement.
///
/// The condition must be a boolean expression.
/// The then branch is always executed if the condition is true.
/// The else branch is optional.
///
/// ─── Type Narrowing Rules ─────────────────────────────────────────────────
///
/// The compiler applies type narrowing inside branches based on the condition.
///
/// **Standard Narrowing — Inside the Block:**
///
/// When the condition checks a nullable variable, the compiler narrows its
/// type inside the then-branch:
///
/// ```lucid
/// const a int? = getValue();
/// 
/// if a != nil {
///     -- a is int here, not int?
///     const x int = a + 1;    -- OK
/// }
/// -- a is still int? here
/// ```
///
/// **Inverse Narrowing — Early Exit Pattern:**
///
/// When a standalone `if` with no `else` contains a control flow exit
/// (`return`, `break`, or `continue`), the compiler applies the inverse
/// of the condition to the rest of the enclosing scope.
///
/// ```lucid
/// -- VALID: standalone if — inverse narrowing applies
/// if a == nil { return }
/// -- a is non-nullable here
///
/// -- INVALID: has else — inverse narrowing NOT applied
/// if a == nil { return } else { log("not nil") }
/// -- a is still int? here
///
/// -- INVALID: chained else-if — inverse narrowing NOT applied
/// if a == nil { return } else if b == nil { return }
/// -- a and b are still nullable here
/// ```
///
/// **Condition Table:**
///
/// | Condition  | Inside block            | Rest of scope (inverse)     |
/// | ---------- | ----------------------- | --------------------------- |
/// | `a == nil` | `a` is `nil`            | `a` is non-nullable         |
/// | `a != nil` | `a` is non-nullable     | `a` is nullable (no change) |
/// | `a == err` | `a` is `err`            | `a` is non-fallible         |
/// | `a != err` | `a` is non-fallible     | `a` is fallible (no change) |
/// | `not a`    | `a` is `nil` or `false` | `a` is non-nullable         |
///
/// **`or` at Top Level — Multiple Variables:**
///
/// ```lucid
/// if a == nil or b == nil { return }
/// -- inverse: a != nil AND b != nil
/// -- rest: a is int, b is string — both narrowed
/// ```
///
/// **`and` at Top Level — No Narrowing:**
///
/// ```lucid
/// if a == nil and b == nil { return }
/// -- inverse: a != nil OR b != nil
/// -- cannot narrow either — no narrowing applied
/// ```
///
/// ```lucid
/// let x int? = getValue()
/// if x == nil {
///     return;
/// }
/// // x is int here (inverse narrowing)
/// ```
///
/// ─── Execution Flow For Type Narrowing ─────────────────────────────────────
///
/// 1. analyzeIfStmt() called
///    │
///    ├── push(ContextKind::IfStmt)
///    ├── setIfConditionCtx(true)
///    ├── checkExpr(condition) → detects x == nil
///    │   └── setPendingNarrowing({x → int, isEquality=true})
///    ├── setIfConditionCtx(false)
///    ├── info = getPendingNarrowing()  // {x → int, isEquality=true}
///    ├── clearPendingNarrowing()
///    │
///    ├── pushNarrowingLevel(false)  // then branch
///    │   └── For equality (x == nil): NO narrowing applied in then branch
///    │       (x is nil, not a definite type)
///    │
///    ├── analyzeBlock(thenBranch)  // ← THIS IS KEY
///    │   │
///    │   ├── pushBlock()
///    │   ├── analyzeStmt(return)  // ← Return statement
///    │   │   │
///    │   │   ├── analyzeReturnStmt()
///    │   │   │   ├── Check: insideFunction() ✅
///    │   │   │   ├── Check: return value matches expected type ✅
///    │   │   │   ├── ctx.contexts.advanceReturnGroup()
///    │   │   │   └── return true  // ← RETURN ALWAYS TRANSFERS CONTROL
///    │   │   │
///    │   │   └── returns true to analyzeBlock
///    │   │
///    │   ├── transfers = true  // ← Set by analyzeReturnStmt
///    │   ├── popBlock()
///    │   └── return transfers  // ← returns true to analyzeIfStmt
///    │
///    ├── thenReturns = true  // ← analyzeBlock returned true
///    ├── popNarrowingLevel()
///    │
///    ├── Check: !stmt->elseBranch ✅
///    ├── Check: thenReturns ✅
///    ├── Check: hasNarrowing ✅
///    ├── Check: info.isEquality ✅
///    │
///    ├── 🔑 setPendingInverseNarrowing(info)  // ← EARLY RETURN DETECTED!
///    │
///    └── pop()  // pop IfStmt context
///
/// 4. analyzeBlock(parentBlock) continues...
///    │
///    ├── pushBlock(parentBlock)
///    │
///    ├── 🔍 hasPendingInverseNarrowing() → true
///    │   │
///    │   ├── pushNarrowingLevel(true)  // Inverse narrowing
///    │   ├── narrowVariable(x, int)   // x is now int in the rest of the block
///    │   ├── clearPendingInverseNarrowing()
///    │   └── hasAppliedPendingNarrowing = true
///    │
///    ├── analyzeStmt(println(x))  // ← x is int here ✅
///    │
///    └── popNarrowingLevel()  // pop inverse narrowing at block exit
///        popBlock()
///
/// @param stmt The if statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool analyzeIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push if context for type narrowing ──────────────────────────
    // Push IfStmt context so checkBinaryExpr knows we're in an if condition
    ctx.contexts.push(ContextKind::IfStmt, const_cast<IfStmtAST*>(stmt), stmt->loc);
    ctx.contexts.setHasElse(stmt->elseBranch != nullptr);

    // ─── 2. Create a bool type for the condition ──────────────────────────
    PrimitiveTypeAST* boolType = ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);

    // ─── 3. Analyze condition with if context ────────────────────────────
    // checkBinaryExpr will detect narrowing patterns because isIfConditionCtx is true
    ctx.contexts.setIfConditionCtx(true);
    
    if (!checkExpr(stmt->condition, boolType, ctx)) {
        ctx.contexts.setIfConditionCtx(false);
        ctx.contexts.pop();
        return false;
    }
    
    ctx.contexts.setIfConditionCtx(false);

    // ─── 4. Extract narrowing info from the condition ────────────────────
    // The condition may contain multiple narrowings (or at top level)
    NarrowingInfo info = extractNarrowingsFromCondition(stmt->condition, ctx);
    bool hasNarrowing = info.hasNarrowing;

    // ─── 5. Analyze then branch with narrowing ──────────────────────────
    bool thenReturns = false;

    if (hasNarrowing) {
        // Push a single narrowing level with all narrowings
        ctx.contexts.pushNarrowingLevel(false);
        
        // Apply all narrowings to the then branch
        for (const auto& [varName, narrowedType] : info.narrowings) {
            // For equality (x == nil), no narrowing in then branch
            // (x is nil, but we don't track nil types)
            // For inequality (x != nil, x != err), apply normal narrowing
            if (!info.isEquality) {
                // x != nil → x is non-nullable
                // x != err → x is non-fallible
                ctx.contexts.narrowVariable(varName, narrowedType);
            }
            // For equality (x == nil), we don't narrow in then branch
            // because x is nil, not a definite type
        }
        
        if (stmt->thenBranch && stmt->thenBranch->isa<BlockStmtAST>()) {
            thenReturns = analyzeBlock(stmt->thenBranch->as<BlockStmtAST>(), ctx);
        } else {
            thenReturns = analyzeStmt(stmt->thenBranch, ctx);
        }
        ctx.contexts.popNarrowingLevel();
    } else {
        // No narrowing - just analyze
        if (stmt->thenBranch && stmt->thenBranch->isa<BlockStmtAST>()) {
            thenReturns = analyzeBlock(stmt->thenBranch->as<BlockStmtAST>(), ctx);
        } else {
            thenReturns = analyzeStmt(stmt->thenBranch, ctx);
        }
    }

    // ─── 6. Analyze else branch (if present) ────────────────────────────
    if (stmt->elseBranch) {
        bool elseReturns = false;

        // ─── 6a. Check if else branch is an else-if ──────────────────────
        if (stmt->elseBranch->isa<IfStmtAST>()) {
            elseReturns = analyzeIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else {
            // ─── 6b. Regular else branch with inverse narrowing ──────────
            if (hasNarrowing) {
                ctx.contexts.pushNarrowingLevel(true); // Inverse narrowing
                
                for (const auto& [varName, narrowedType] : info.narrowings) {
                    // For equality (x == nil, x == err):
                    //   x is non-nullable/non-fallible in else branch
                    // For inequality (x != nil, x != err):
                    //   x is nullable/fallible (no change), so skip
                    if (info.isEquality) {
                        ctx.contexts.narrowVariable(varName, narrowedType);
                    }
                }
                
                if (stmt->elseBranch->isa<BlockStmtAST>()) {
                    elseReturns = analyzeBlock(stmt->elseBranch->as<BlockStmtAST>(), ctx);
                } else {
                    elseReturns = analyzeStmt(stmt->elseBranch, ctx);
                }
                ctx.contexts.popNarrowingLevel();
            } else {
                // No narrowing
                if (stmt->elseBranch->isa<BlockStmtAST>()) {
                    elseReturns = analyzeBlock(stmt->elseBranch->as<BlockStmtAST>(), ctx);
                } else {
                    elseReturns = analyzeStmt(stmt->elseBranch, ctx);
                }
            }
        }

        // If both branches return, the if transfers control
        if (thenReturns && elseReturns) {
            ctx.contexts.pop();
            return true;
        }
    }

    // ─── 7. Handle inverse narrowing for standalone if ───────────────────
    // Rule: standalone if with early exit applies inverse narrowing to rest of scope
    // ONLY when: no else, then branch transfers control, and condition is equality
    if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
        // Set pending inverse narrowing on the current block
        // This will be applied when analyzeBlock continues
        ctx.contexts.setPendingInverseNarrowing(info);
    }

    // ─── 8. Pop if context ────────────────────────────────────────────────
    ctx.contexts.pop();

    // If then branch doesn't transfer control, the if doesn't transfer
    return false;
}

// =============================================================================
// analyzeSwitchStmt
// =============================================================================

/// @brief Analyze a switch statement.
///
/// The subject must be an integer, enum, bool, char, or string type.
/// Each case must have a body that is a block.
/// The default clause is optional.
///
/// @param stmt The switch statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool analyzeSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check the subject expression (must be integer, enum, bool, char, or string)
    // TODO: Analyze each case statement
    // TODO: Check for duplicate case values (compile-time error)
    // TODO: Check for exhaustive matching (enum types without default)
    // TODO: Analyze the default clause (if present)
    // TODO: Return true if ALL cases transfer control AND default transfers control
    //       (or no default but all possible values are covered)
    return false;
}

// =============================================================================
// analyzeSwitchCase
// =============================================================================

/// @brief Analyze a switch case.
///
/// A case has one or more match values (literals, enum variants, or ranges)
/// and a body block.
///
/// @param switchCase The switch case.
/// @param ctx The semantic context.
/// @return true if the case body guarantees control transfer out of the block.
bool analyzeSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx) {
    // TODO: Check each case value (must be compatible with the switch subject)
    // TODO: Check for duplicate case values within the same switch
    // TODO: Analyze the case body
    // TODO: Return true if the body transfers control
    return false;
}

// =============================================================================
// analyzeForStmt
// =============================================================================

/// @brief Analyze a for loop statement.
///
/// A for loop iterates over a range or collection.
/// The index and value bindings are optional (can be ignored with `_`).
///
/// @param stmt The for statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool analyzeForStmt(const ForStmtAST* stmt, SemaContext& ctx) {
    // TODO: Push a new scope for loop variables
    // TODO: Analyze the index binding (if present)
    // TODO: Analyze the value binding (if present)
    // TODO: Analyze the iterable expression (must be a range or collection)
    // TODO: Analyze the step expression (if present, must be numeric)
    // TODO: Analyze the loop body
    // TODO: Return false (for loops do NOT guarantee transfer)
    return false;
}

// =============================================================================
// analyzeWhileStmt
// =============================================================================

/// @brief Analyze a while loop statement.
///
/// The condition is tested before each iteration.
/// The loop exits when the condition is false or a break is reached.
///
/// @param stmt The while statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool analyzeWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check the condition expression (must be bool)
    // TODO: Analyze the loop body
    // TODO: Return false (while loops do NOT guarantee transfer)
    return false;
}

// =============================================================================
// analyzeDoWhileStmt
// =============================================================================

/// @brief Analyze a do-while loop statement.
///
/// The body executes at least once before the condition is checked.
///
/// @param stmt The do-while statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool analyzeDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx) {
    // TODO: Analyze the loop body
    // TODO: Check the condition expression (must be bool)
    // TODO: Return false (do-while loops do NOT guarantee transfer)
    return false;
}

// =============================================================================
// analyzeReturnStmt
// =============================================================================

/// @brief Analyze a return statement.
///
/// The return statement exits the enclosing function.
/// It may return zero or one expression (Lucid uses single expression returns).
///
/// ─── Validation ──────────────────────────────────────────────────────────────
/// 1. Must be inside a function body (ContextKind::FuncBody)
/// 2. The return value type must match the function's return type
/// 3. Cannot return from top-level
/// 4. Inside a standalone if with no else, the return triggers inverse narrowing
///    (handled by analyzeIfStmt via the boolean return value)
///
/// @param stmt The return statement.
/// @param ctx The semantic context.
/// @return true (return always transfers control out of the block).
bool analyzeReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;  // Return always transfers control

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "return statement outside of function body");
        return true;  // Still transfers control (error recovery)
    }

    // ─── 2. Get the current function's return requirements ──────────────────
    const ReturnRequirements* reqs = ctx.contexts.currentReturnReqs();
    if (!reqs) {
        // Shouldn't happen if insideFunction() is true, but safe
        ctx.error(stmt, DiagCode::E3006,
                  "return statement with no return requirements");
        return true;
    }

    // ─── 3. Get the current return group ────────────────────────────────────
    // For curried functions, each return must match the current group's type
    const ReturnRequirements::Group* currentGroup = ctx.contexts.currentReturnGroup();

    // ─── 4. Check return value against current group ────────────────────────
    if (stmt->value) {
        // ── 4a. Function has a return value ─────────────────────────────────
        
        // Must have a current group to return to
        if (!currentGroup) {
            ctx.error(stmt, DiagCode::E3005,
                      "return value provided but function has no pending return group");
            return true;
        }

        // Check the return value type against the group's return type
        const TypeAST* expectedType = currentGroup->returnType;
        if (!expectedType) {
            ctx.error(stmt, DiagCode::E3005,
                      "return value provided but function expects void return");
            return true;
        }

        // Check the expression against the expected type
        if (!checkExpr(stmt->value, expectedType, ctx)) {
            // Error already reported by checkExpr
            return true;
        }

        // ── 4b. Validate fallible/nullable propagation ──────────────────────
        // A fallible value cannot be returned without handling err first
        if (stmt->value->valueState == ValueState::Err) {
            if (!isFallibleType(expectedType)) {
                ctx.error(stmt->value, DiagCode::E3003,
                          "cannot return err to non-fallible return type");
                return true;
            }
            // If target is fallible, err is acceptable
        }

        if (stmt->value->valueState == ValueState::Nil) {
            if (!isNullableType(expectedType)) {
                ctx.error(stmt->value, DiagCode::E3003,
                          "cannot return nil to non-nullable return type");
                return true;
            }
            // If target is nullable, nil is acceptable
        }

    } else {
        // ── 4c. Void return (no value) ──────────────────────────────────────
        
        // Check if void is allowed
        if (currentGroup && currentGroup->requiresReturn) {
            // This group requires a return value
            ctx.error(stmt, DiagCode::E3005,
                      "void return statement but function expects a return value");
            return true;
        }

        // Check if the function allows optional return
        if (!reqs->allowsOptionalReturn) {
            ctx.error(stmt, DiagCode::E3005,
                      "void return statement not allowed in this function");
            return true;
        }
    }

    // ─── 5. Advance the return group ────────────────────────────────────────
    // This marks the current group as satisfied and moves to the next
    if (currentGroup) {
        ctx.contexts.advanceReturnGroup();
        
        // Record where this group was satisfied (for diagnostics)
        // Note: currentGroup is const, we need to cast
        const_cast<ReturnRequirements::Group*>(currentGroup)->satisfiedAt = stmt->loc;
    }

    // ─── 6. Check if all groups are now satisfied ──────────────────────────
    if (reqs->hasRequirements() && reqs->isSatisfied()) {
        // All return requirements satisfied - this is a terminal return
        // (the function will exit after this)
        // No additional action needed - the return already transfers control
    }

    // ─── 7. Return true (return always transfers control) ───────────────────
    // This boolean propagates up to analyzeIfStmt, which checks
    // if thenReturns is true to apply inverse narrowing
    return true;
}

// =============================================================================
// analyzeBreakStmt
// =============================================================================

/// @brief Analyze a break statement.
///
/// The break statement exits the nearest enclosing loop or switch.
///
/// @param stmt The break statement.
/// @param ctx The semantic context.
/// @return true (break always transfers control out of the block).
bool analyzeBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check that we're inside a loop or switch (SemanticContext::LoopBody or SwitchBody)
    // TODO: Verify break is not used outside of a loop/switch
    // TODO: Return true (break always transfers control)
    return true;
}

// =============================================================================
// analyzeContinueStmt
// =============================================================================

/// @brief Analyze a continue statement.
///
/// The continue statement skips the rest of the current loop iteration
/// and jumps to the next iteration.
///
/// @param stmt The continue statement.
/// @param ctx The semantic context.
/// @return true (continue always transfers control out of the block).
bool analyzeContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check that we're inside a loop (SemanticContext::LoopBody)
    // TODO: Verify continue is not used outside of a loop
    // TODO: Return true (continue always transfers control)
    return true;
}

// =============================================================================
// analyzeExprStmt
// =============================================================================

/// @brief Analyze an expression statement.
///
/// An expression statement evaluates an expression for its side effects.
/// The expression's value is discarded.
///
/// @param stmt The expression statement.
/// @param ctx The semantic context.
/// @return false (expression statements do NOT transfer control).
bool analyzeExprStmt(const ExprStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check the expression (validate it exists and has a type)
    // TODO: Warn if the expression has no side effects (pure expression)
    // TODO: Warn if a non-void expression result is discarded without explicit intent
    // TODO: Return false (expression statements do not transfer control)
    return false;
}

// =============================================================================
// analyzeDeclStmt
// =============================================================================

/// @brief Analyze a declaration statement.
///
/// A declaration statement introduces one or more local declarations
/// (variables, functions, structs, enums, traits).
///
/// @param stmt The declaration statement.
/// @param ctx The semantic context.
/// @return false (declaration statements do NOT transfer control).
bool analyzeDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx) {
    // TODO: Dispatch to analyzeDecl() for the inner declaration
    // TODO: Return false (declarations do not transfer control)
    return false;
}




// =============================================================================
// Concurrency Statements
// =============================================================================

// ─── analyzeAsyncStmt ──────────────────────────────────────────────────────

/// @brief Analyze an async statement.
///
/// Grammar: `async IDENTIFIER { ',' IDENTIFIER } '=' call_expr`
///
/// Schedules a function call on the event loop.
/// The result is a Future<T> that must be awaited.
///
/// @param stmt The async statement.
/// @param ctx The semantic context.
/// @return false (async does NOT transfer control).
bool analyzeAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check that we're inside a function body
    // TODO: Check each target variable (must be assignable lvalue)
    // TODO: Check the call expression (must be a function call)
    // TODO: Check the call expression's return type is compatible with targets
    // TODO: Mark variables as Future<T>
    // TODO: Return false (async does not transfer control)
    return false;
}

// ─── analyzeAwaitStmt ──────────────────────────────────────────────────────

/// @brief Analyze an await statement.
///
/// Grammar: `await IDENTIFIER { ',' IDENTIFIER }`
///
/// Waits for async operations to complete.
/// After await, variables become plain T (no longer Future<T>).
///
/// @param stmt The await statement.
/// @param ctx The semantic context.
/// @return false (await does NOT transfer control).
bool analyzeAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check that we're inside a function body
    // TODO: Check each target variable (must be assignable lvalue)
    // TODO: Verify each variable is Future<T>
    // TODO: Change variable type from Future<T> to T
    // TODO: Return false (await does not transfer control)
    return false;
}

// ─── analyzeSpawnStmt ──────────────────────────────────────────────────────

/// @brief Analyze a spawn statement.
///
/// Grammar: `spawn IDENTIFIER { ',' IDENTIFIER } '=' call_expr`
///
/// Launches a function call on a separate OS thread.
/// The result is a Future<T> that must be joined.
///
/// @param stmt The spawn statement.
/// @param ctx The semantic context.
/// @return false (spawn does NOT transfer control).
bool analyzeSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check that we're inside a function body
    // TODO: Check each target variable (must be assignable lvalue)
    // TODO: Check the call expression (must be a function call)
    // TODO: Check the call expression's return type is compatible with targets
    // TODO: Mark variables as Future<T>
    // TODO: Warn about spawns that are never joined
    // TODO: Return false (spawn does not transfer control)
    return false;
}

// ─── analyzeJoinStmt ───────────────────────────────────────────────────────

/// @brief Analyze a join statement.
///
/// Grammar: `join IDENTIFIER { ',' IDENTIFIER }`
///
/// Waits for spawned threads to complete.
/// After join, variables become plain T (no longer Future<T>).
///
/// @param stmt The join statement.
/// @param ctx The semantic context.
/// @return false (join does NOT transfer control).
bool analyzeJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx) {
    // TODO: Check that we're inside a function body
    // TODO: Check each target variable (must be assignable lvalue)
    // TODO: Verify each variable is Future<T> from spawn
    // TODO: Change variable type from Future<T> to T
    // TODO: Return false (join does not transfer control)
    return false;
}

} // namespace sema