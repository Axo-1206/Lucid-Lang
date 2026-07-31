/// @file SemaStmt.cpp
/// @brief Implements Sema.hpp's "STATEMENTS - Control flow analysis" section.
/// 
/// This file handles Phase 2: Type resolution and validation for statements.
/// All names are already registered from Phase 1, so lookups will succeed.
/// 
/// @architectural_note Control Flow Analysis
///   Each statement resolver returns a boolean indicating whether the statement
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
/// 
/// @architectural_note Expression Resolution with Target Type
///   Statements that expect a specific type (conditions, loop bounds, etc.)
///   pass the expected type as targetType to `resolveExprWithTarget()`.
///   This centralizes type checking and uses cached type instances.

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/context/ReturnRequirements.hpp"

namespace sema {

// =============================================================================
// resolveStmt - Dispatch
// =============================================================================

/// @brief Dispatch a statement to its specific resolver function.
///
/// @param stmt The statement to resolve.
/// @param ctx The semantic context.
/// @return true if this statement guarantees control transfer out of the block.
bool resolveStmt(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:        return resolveBlock(stmt->as<BlockStmtAST>(), ctx);
        case ASTKind::IfStmt:           return resolveIfStmt(stmt->as<IfStmtAST>(), ctx);
        case ASTKind::SwitchStmt:       return resolveSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
        case ASTKind::SwitchCase:       return resolveSwitchCase(stmt->as<SwitchCaseAST>(), ctx);
        case ASTKind::ForStmt:          return resolveForStmt(stmt->as<ForStmtAST>(), ctx);
        case ASTKind::WhileStmt:        return resolveWhileStmt(stmt->as<WhileStmtAST>(), ctx);
        case ASTKind::DoWhileStmt:      return resolveDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
        case ASTKind::ReturnStmt:       return resolveReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
        case ASTKind::BreakStmt:        return resolveBreakStmt(stmt->as<BreakStmtAST>(), ctx);
        case ASTKind::ContinueStmt:     return resolveContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
        case ASTKind::ExprStmt:         return resolveExprStmt(stmt->as<ExprStmtAST>(), ctx);
        case ASTKind::DeclStmt:         return resolveDeclStmt(stmt->as<DeclStmtAST>(), ctx);
        case ASTKind::AsyncExpr:        return resolveAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
        case ASTKind::AwaitExpr:        return resolveAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
        case ASTKind::SpawnExpr:        return resolveSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
        case ASTKind::JoinExpr:         return resolveJoinStmt(stmt->as<JoinStmtAST>(), ctx);
        default:
            return false;
    }
}

// =============================================================================
// resolveBlock
// =============================================================================

/// @brief Resolve a block statement.
///
/// A block is a sequence of statements in a new scope.
/// The block returns true if its last statement guarantees control transfer.
///
/// @param block The block statement.
/// @param ctx The semantic context.
/// @return true if the block guarantees control transfer out of the block.
bool resolveBlock(const BlockStmtAST* block, SemaContext& ctx) {
    // ─── 1. Push block context ────────────────────────────────────────────
    ctx.contexts.pushBlock(const_cast<BlockStmtAST*>(block), block->loc);

    bool transfers = false;
    bool hasAppliedPendingNarrowing = false;

    // ─── 2. Apply pending inverse narrowing ──────────────────────────────
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

    // ─── 3. Push scope for the block ──────────────────────────────────────
    ctx.pushScope();

    // ─── 4. Resolve each statement ─────────────────────────────────────────
    for (const StmtAST* stmt : block->stmts) {
        if (transfers) {
            // TODO: Emit unreachable code warning
            continue;
        }
        transfers = resolveStmt(stmt, ctx);
        if (transfers) {
            break;
        }
    }

    // ─── 5. Check for unresolved async/spawn operations ────────────────────
    // This is the critical part for concurrency tracking
    for (const InternedString& name : ctx.getPendingAsyncNames()) {
        ctx.warning(block, DiagCode::W1003,
                    "async '", ctx.pool.lookup(name), "' was never awaited");
    }

    for (const InternedString& name : ctx.getPendingSpawnNames()) {
        ctx.warning(block, DiagCode::W1004,
                    "spawn '", ctx.pool.lookup(name), "' was never joined");
    }

    // ─── 6. Pop scope ─────────────────────────────────────────────────────
    ctx.popScope();

    // ─── 7. Pop pending narrowing level ────────────────────────────────────
    if (hasAppliedPendingNarrowing) {
        ctx.contexts.popNarrowingLevel();
    }

    // ─── 8. Pop block context ──────────────────────────────────────────────
    ctx.contexts.pop();

    // ─── 9. Final Check: Return Requirements ─────────────────────────────
    if (ctx.contexts.hasReturnRequirements() && !ctx.contexts.returnRequirementsSatisfied()) {
        if (!transfers) {
            ctx.error(block, DiagCode::E3005,
                      "function is missing a return statement");
        }
    }

    return transfers;
}

// =============================================================================
// resolveIfStmt
// =============================================================================

/// @brief Resolve an if statement.
///
/// The condition must be a boolean expression.
/// The then branch is always executed if the condition is true.
/// The else branch is optional.
///
/// ─── Type Narrowing Rules ─────────────────────────────────────────────────
///
/// The compiler applies type narrowing inside branches based on the condition.
/// See TypeNarrowHelpers.hpp for detailed documentation.
bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push if context for type narrowing ──────────────────────────
    ctx.contexts.push(ContextKind::IfStmt, const_cast<IfStmtAST*>(stmt), stmt->loc);
    ctx.contexts.setHasElse(stmt->elseBranch != nullptr);

    // ─── 2. Resolve condition with target type = bool ────────────────────
    // Use cached bool type from context
    PrimitiveTypeAST* boolType = ctx.getBoolType();

    // Enable if condition context for narrowing detection
    ctx.contexts.setIfConditionCtx(true);
    
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    ctx.contexts.setIfConditionCtx(false);

    // Check if condition resolved correctly
    if (!condType || condType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        ctx.contexts.pop();
        return false;
    }

    // ─── 3. Extract narrowing info from the condition ────────────────────
    NarrowingInfo info = extractNarrowingsFromCondition(stmt->condition, ctx);
    bool hasNarrowing = info.hasNarrowing;

    // ─── 4. Resolve then branch with narrowing ──────────────────────────
    bool thenReturns = false;

    if (hasNarrowing) {
        ctx.contexts.pushNarrowingLevel(false);
        
        for (const auto& [varName, narrowedType] : info.narrowings) {
            // For equality (x == nil), no narrowing in then branch
            // For inequality (x != nil, x != err), apply normal narrowing
            if (!info.isEquality) {
                ctx.contexts.narrowVariable(varName, narrowedType);
            }
        }
        
        if (stmt->thenBranch && stmt->thenBranch->isa<BlockStmtAST>()) {
            thenReturns = resolveBlock(stmt->thenBranch->as<BlockStmtAST>(), ctx);
        } else {
            thenReturns = resolveStmt(stmt->thenBranch, ctx);
        }
        ctx.contexts.popNarrowingLevel();
    } else {
        if (stmt->thenBranch && stmt->thenBranch->isa<BlockStmtAST>()) {
            thenReturns = resolveBlock(stmt->thenBranch->as<BlockStmtAST>(), ctx);
        } else {
            thenReturns = resolveStmt(stmt->thenBranch, ctx);
        }
    }

    // ─── 5. Resolve else branch (if present) ────────────────────────────
    if (stmt->elseBranch) {
        bool elseReturns = false;

        if (stmt->elseBranch->isa<IfStmtAST>()) {
            elseReturns = resolveIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else {
            if (hasNarrowing) {
                ctx.contexts.pushNarrowingLevel(true); // Inverse narrowing
                
                for (const auto& [varName, narrowedType] : info.narrowings) {
                    // For equality (x == nil, x == err):
                    //   x is non-nullable/non-fallible in else branch
                    if (info.isEquality) {
                        ctx.contexts.narrowVariable(varName, narrowedType);
                    }
                }
                
                if (stmt->elseBranch->isa<BlockStmtAST>()) {
                    elseReturns = resolveBlock(stmt->elseBranch->as<BlockStmtAST>(), ctx);
                } else {
                    elseReturns = resolveStmt(stmt->elseBranch, ctx);
                }
                ctx.contexts.popNarrowingLevel();
            } else {
                if (stmt->elseBranch->isa<BlockStmtAST>()) {
                    elseReturns = resolveBlock(stmt->elseBranch->as<BlockStmtAST>(), ctx);
                } else {
                    elseReturns = resolveStmt(stmt->elseBranch, ctx);
                }
            }
        }

        if (thenReturns && elseReturns) {
            ctx.contexts.pop();
            return true;
        }
    }

    // ─── 6. Handle inverse narrowing for standalone if ───────────────────
    if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
        ctx.contexts.setPendingInverseNarrowing(info);
    }

    // ─── 7. Pop if context ────────────────────────────────────────────────
    ctx.contexts.pop();
    return false;
}

// =============================================================================
// resolveSwitchStmt
// =============================================================================

/// @brief Resolve a switch statement.
///
/// The subject must be an integer, enum, bool, char, or string type.
/// Each case must have a body that is a block.
/// The default clause is optional.
///
/// @param stmt The switch statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx) {
    // ─── 1. Resolve subject expression (no target type yet) ──────────────
    TypeAST* subjectType = resolveExpr(stmt->subject, ctx);
    if (!subjectType || subjectType->isa<UnknownTypeAST>()) {
        ctx.error(stmt->subject, DiagCode::E2002, "switch subject has unknown type");
        ctx.contexts.push(ContextKind::SwitchBody, const_cast<SwitchStmtAST*>(stmt), stmt->loc);
        ctx.contexts.pop();
        return false;
    }
    
    // ─── 2. Validate subject type ──────────────────────────────────────
    if (!isValidSwitchType(subjectType, ctx)) {
        ctx.error(stmt->subject, DiagCode::E3003,
                  "switch subject must be integer, enum, bool, char, or string");
        return false;
    }
    
    // ─── 3. Push switch context ──────────────────────────────────────
    ctx.contexts.push(ContextKind::SwitchBody, const_cast<SwitchStmtAST*>(stmt), stmt->loc);
    
    // ─── 4. Validate cases ─────────────────────────────────────────────
    bool allCasesReturn = true;
    
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        // Validate each case value against the subject type
        for (ExprAST* value : caseStmt->values) {
            // Resolve case value against the subject type
            TypeAST* valueType = resolveExprWithTarget(value, subjectType, ctx);
            if (!valueType || valueType->isa<UnknownTypeAST>()) {
                // Error already reported by resolveExprWithTarget
                continue;
            }
            
            // Check if the case value is compatible with the subject type
            if (!isSwitchCaseCompatible(value, subjectType, ctx)) {
                ctx.error(value, DiagCode::E3003,
                          "case value is not compatible with switch subject type");
            }
        }
        
        // Resolve case body
        if (caseStmt->body) {
            if (!resolveBlock(caseStmt->body, ctx)) {
                allCasesReturn = false;
            }
        }
    }
    
    // ─── 5. Check exhaustiveness ──────────────────────────────────────
    if (!stmt->defaultBody && isEnumType(subjectType, ctx)) {
        switch_helpers::checkExhaustiveness(stmt, subjectType, ctx);
    }
    
    // ─── 6. Resolve default clause ────────────────────────────────────
    if (stmt->defaultBody) {
        if (!resolveBlock(stmt->defaultBody, ctx)) {
            allCasesReturn = false;
        }
    }
    
    ctx.contexts.pop();
    
    return allCasesReturn && (stmt->defaultBody || !isEnumType(subjectType, ctx));
}

// =============================================================================
// resolveSwitchCase
// =============================================================================

/// @brief Resolve a switch case.
///
/// A case has one or more match values (literals, enum variants, or ranges)
/// and a body block.
///
/// @param switchCase The switch case.
/// @param ctx The semantic context.
/// @return true if the case body guarantees control transfer out of the block.
bool resolveSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx) {
    if (!switchCase) return false;

    // ─── 1. Validate each case value ──────────────────────────────────────
    for (ExprAST* value : switchCase->values) {
        if (!value->isa<LiteralExprAST>() && 
            !value->isa<FieldAccessExprAST>() && 
            !value->isa<RangeExprAST>()) {
            ctx.error(value, DiagCode::E3003,
                      "case value must be a literal, enum variant, or range");
            continue;
        }

        // Resolve the case value (no target type - it'll be checked by the switch)
        TypeAST* valueType = resolveExpr(value, ctx);
        if (!valueType || valueType->isa<UnknownTypeAST>()) {
            ctx.error(value, DiagCode::E3003, "case value has unknown type");
            continue;
        }
    }

    // ─── 2. Resolve the case body ────────────────────────────────────────
    if (switchCase->body) {
        return resolveBlock(switchCase->body, ctx);
    }

    return false;
}

// =============================================================================
// resolveForStmt
// =============================================================================

/// @brief Resolve a for loop statement.
///
/// A for loop iterates over a range or collection.
/// The index and value bindings are optional (can be ignored with `_`).
///
/// @param stmt The for statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveForStmt(const ForStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push loop context ────────────────────────────────────────────
    ctx.contexts.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Push a scope for loop variables ──────────────────────────────
    ctx.pushScope();

    // ─── 3. Resolve the index binding (if present) ──────────────────────
    if (stmt->indexVar) {
        TypeAST* indexType = resolveType(stmt->indexVar->type, ctx);
        if (!indexType) {
            // Error already reported
        }
        if (indexType && !isIntegerType(indexType)) {
            ctx.error(stmt->indexVar, DiagCode::E3003,
                      "index variable must be an integer type");
        }
    }

    // ─── 4. Resolve the value binding (if present) ──────────────────────
    if (stmt->valueVar) {
        TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
        if (!valueType) {
            // Error already reported
        }
    }

    // ─── 5. Resolve the iterable expression ──────────────────────────────
    if (stmt->iterable) {
        // For range loops, the iterable is a RangeExprAST
        // For collection loops, it's an array
        // We resolve it without a target type first
        TypeAST* iterableType = resolveExpr(stmt->iterable, ctx);
        if (!iterableType || iterableType->isa<UnknownTypeAST>()) {
            ctx.error(stmt->iterable, DiagCode::E3003, "iterable has unknown type");
            ctx.popScope();
            ctx.contexts.pop();
            return false;
        }
        
        // TODO: Validate iterable type (array or range)
        // If it's a range, it should be numeric
        // If it's an array, it should have a valid element type
    }

    // ─── 6. Resolve the step expression (if present) ──────────────────────
    if (stmt->step) {
        // Step must be numeric
        // Use cached int type as target
        PrimitiveTypeAST* numericType = ctx.getIntType();
        TypeAST* stepType = resolveExprWithTarget(stmt->step, numericType, ctx);
        if (!stepType || stepType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            ctx.popScope();
            ctx.contexts.pop();
            return false;
        }
    }

    // ─── 7. Resolve the loop body ─────────────────────────────────────────
    bool bodyTransfers = false;
    if (stmt->body) {
        bodyTransfers = resolveStmt(stmt->body, ctx);
    }

    // ─── 8. Pop the scope ──────────────────────────────────────────────────
    ctx.popScope();

    // ─── 9. Pop loop context ──────────────────────────────────────────────
    ctx.contexts.pop();

    // For loops do NOT guarantee control transfer
    return false;
}

// =============================================================================
// resolveWhileStmt
// =============================================================================

/// @brief Resolve a while loop statement.
///
/// The condition is tested before each iteration.
/// The loop exits when the condition is false or a break is reached.
///
/// @param stmt The while statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push loop context ────────────────────────────────────────────
    ctx.contexts.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Resolve the condition against bool type ──────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        ctx.contexts.pop();
        return false;
    }

    // ─── 3. Resolve the loop body ─────────────────────────────────────────
    bool bodyTransfers = false;
    if (stmt->body) {
        bodyTransfers = resolveStmt(stmt->body, ctx);
    }

    // ─── 4. Pop loop context ──────────────────────────────────────────────
    ctx.contexts.pop();

    return false;
}

// =============================================================================
// resolveDoWhileStmt
// =============================================================================

/// @brief Resolve a do-while loop statement.
///
/// The body executes at least once before the condition is checked.
///
/// @param stmt The do-while statement.
/// @param ctx The semantic context.
/// @return true if the statement guarantees control transfer out of the block.
bool resolveDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push loop context ────────────────────────────────────────────
    ctx.contexts.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Resolve the loop body ─────────────────────────────────────────
    bool bodyTransfers = false;
    if (stmt->body) {
        bodyTransfers = resolveStmt(stmt->body, ctx);
    }

    // ─── 3. Resolve the condition against bool type ──────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        ctx.contexts.pop();
        return false;
    }

    // ─── 4. Pop loop context ──────────────────────────────────────────────
    ctx.contexts.pop();

    return false;
}

// =============================================================================
// resolveReturnStmt
// =============================================================================

/// @brief Resolve a return statement.
///
/// The return statement exits the enclosing function.
///
/// @param stmt The return statement.
/// @param ctx The semantic context.
/// @return true (return always transfers control out of the block).
bool resolveReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "return statement outside of function body");
        return true;
    }

    // ─── 2. Get the current function's return requirements ──────────────────
    const ReturnRequirements* reqs = ctx.contexts.currentReturnReqs();
    if (!reqs) {
        ctx.error(stmt, DiagCode::E3006,
                  "return statement with no return requirements");
        return true;
    }

    // ─── 3. Get the current return group ────────────────────────────────────
    const ReturnRequirements::Group* currentGroup = ctx.contexts.currentReturnGroup();

    // ─── 4. Check return value against current group ────────────────────────
    if (stmt->value) {
        if (!currentGroup) {
            ctx.error(stmt, DiagCode::E3005,
                      "return value provided but function has no pending return group");
            return true;
        }

        const TypeAST* expectedType = currentGroup->returnType;
        if (!expectedType) {
            ctx.error(stmt, DiagCode::E3005,
                      "return value provided but function expects void return");
            return true;
        }

        // Resolve the return value against the expected type
        TypeAST* valueType = resolveExprWithTarget(stmt->value, expectedType, ctx);
        if (!valueType || valueType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            return true;
        }

        // Validate fallible/nullable propagation
        if (stmt->value->valueState == ValueState::Err) {
            if (!isFallibleType(expectedType)) {
                ctx.error(stmt->value, DiagCode::E3003,
                          "cannot return err to non-fallible return type");
                return true;
            }
        }

        if (stmt->value->valueState == ValueState::Nil) {
            if (!isNullableType(expectedType)) {
                ctx.error(stmt->value, DiagCode::E3003,
                          "cannot return nil to non-nullable return type");
                return true;
            }
        }

    } else {
        // ── 4b. Void return (no value) ──────────────────────────────────────
        if (currentGroup && currentGroup->requiresReturn) {
            ctx.error(stmt, DiagCode::E3005,
                      "void return statement but function expects a return value");
            return true;
        }

        if (!reqs->allowsOptionalReturn) {
            ctx.error(stmt, DiagCode::E3005,
                      "void return statement not allowed in this function");
            return true;
        }
    }

    // ─── 5. Advance the return group ────────────────────────────────────────
    if (currentGroup) {
        ctx.contexts.advanceReturnGroup();
        const_cast<ReturnRequirements::Group*>(currentGroup)->satisfiedAt = stmt->loc;
    }

    return true;
}

// =============================================================================
// resolveBreakStmt
// =============================================================================

/// @brief Resolve a break statement.
///
/// The break statement exits the nearest enclosing loop or switch.
///
/// @param stmt The break statement.
/// @param ctx The semantic context.
/// @return true (break always transfers control out of the block).
bool resolveBreakStmt(const BreakStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.contexts.insideLoop() && !ctx.contexts.insideSwitch()) {
        ctx.error(stmt, DiagCode::E3006,
                  "break statement outside of loop or switch");
        return true;
    }

    return true;
}

// =============================================================================
// resolveContinueStmt
// =============================================================================

/// @brief Resolve a continue statement.
///
/// The continue statement skips the rest of the current loop iteration.
///
/// @param stmt The continue statement.
/// @param ctx The semantic context.
/// @return true (continue always transfers control out of the block).
bool resolveContinueStmt(const ContinueStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    if (!ctx.contexts.insideLoop()) {
        ctx.error(stmt, DiagCode::E3006,
                  "continue statement outside of loop");
        return true;
    }

    return true;
}

// =============================================================================
// resolveExprStmt
// =============================================================================

/// @brief Resolve an expression statement.
///
/// An expression statement evaluates an expression for its side effects.
/// The expression's value is discarded.
///
/// @param stmt The expression statement.
/// @param ctx The semantic context.
/// @return false (expression statements do NOT transfer control).
bool resolveExprStmt(const ExprStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->expr) return false;

    // ─── 1. Resolve the expression (no target type) ──────────────────────
    TypeAST* exprType = resolveExpr(stmt->expr, ctx);
    if (!exprType || exprType->isa<UnknownTypeAST>()) {
        ctx.error(stmt->expr, DiagCode::E3003, "expression has unknown type");
        return false;
    }

    // ─── 2. Check for discarded non-void value ─────────────────────────────
    // If the expression has a non-void type and no side effects, warn
    if (exprType && !exprType->isa<UnknownTypeAST>()) {
        // TODO: Check if expression has side effects
        // bool hasSideEffects = hasSideEffects(stmt->expr, ctx);
        // if (!hasSideEffects) {
        //     ctx.warning(stmt, DiagCode::W1002,
        //                 "expression result is discarded (no side effects)");
        // }
    }

    return false;
}

// =============================================================================
// resolveDeclStmt
// =============================================================================

/// @brief Resolve a declaration statement.
///
/// A declaration statement introduces one or more local declarations.
///
/// @param stmt The declaration statement.
/// @param ctx The semantic context.
/// @return false (declaration statements do NOT transfer control).
bool resolveDeclStmt(const DeclStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->decl) return false;

    resolveDecl(stmt->decl, ctx);
    return false;
}

// =============================================================================
// Concurrency Statements
// =============================================================================

// ─── resolveAsyncStmt ──────────────────────────────────────────────────────

bool resolveAsyncStmt(const AsyncStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "async statement outside of function body");
        return false;
    }

    // ─── 2. Check the target variable ──────────────────────────────────────
    if (!stmt->target) {
        ctx.error(stmt, DiagCode::E3003, "async statement requires a target variable");
        return false;
    }

    if (!stmt->target->isa<IdentifierExprAST>()) {
        ctx.error(stmt->target, DiagCode::E3003,
                  "async target must be a variable (not an expression)");
        return false;
    }

    const IdentifierExprAST* target = stmt->target->as<IdentifierExprAST>();
    InternedString targetName = target->name;

    // ─── 3. Check for `_` discard ──────────────────────────────────────────
    // `_` is a valid target - it means fire and forget, no tracking needed
    // But we need to ensure the variable exists and is valid
    if (targetName.id != 0) {  // Not `_`
        // Verify the variable exists
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.error(stmt->target, DiagCode::E2001,
                      "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        // Verify it's a variable (not a function, enum, etc.)
        if (!decl->isa<VarDeclAST>()) {
            ctx.error(stmt->target, DiagCode::E3003,
                      "'", ctx.pool.lookup(targetName), "' is not a variable");
            return false;
        }
    }

    // ─── 4. Resolve the call expression ─────────────────────────────────────
    if (!stmt->call) {
        ctx.error(stmt, DiagCode::E3003, "async statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExpr(stmt->call, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExpr
        return false;
    }

    // ─── 5. Store in pending list if not `_` ──────────────────────────────
    if (targetName.id != 0) {  // Not `_`
        ctx.addPendingAsync(targetName, stmt->call, stmt->loc);
    }

    return false;  // async does not transfer control
}

// ─── resolveAwaitStmt ──────────────────────────────────────────────────────

bool resolveAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "await statement outside of function body");
        return false;
    }

    // ─── 2. Check each target variable ─────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.error(target, DiagCode::E3003,
                      "await target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── 3. Check if this is a pending async operation ────────────────
        if (ctx.hasPendingAsync(targetName)) {
            // Resolve the async operation
            ctx.resolveAsync(targetName);
        } else {
            ctx.error(target, DiagCode::E3003,
                      "'", ctx.pool.lookup(targetName), "' was not declared with async");
            return false;
        }

        // ─── 4. Verify the variable exists ─────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.error(target, DiagCode::E2001,
                      "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        // TODO: Change variable type from Future<T> to T
        // This would require updating the declaration's type
    }

    return false;  // await does not transfer control
}

// ─── resolveSpawnStmt ──────────────────────────────────────────────────────

bool resolveSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "spawn statement outside of function body");
        return false;
    }

    // ─── 2. Check the target variable ──────────────────────────────────────
    InternedString targetName;
    bool isDiscard = false;

    if (stmt->target) {
        if (!stmt->target->isa<IdentifierExprAST>()) {
            ctx.error(stmt->target, DiagCode::E3003,
                      "spawn target must be a variable (not an expression)");
            return false;
        }

        const IdentifierExprAST* target = stmt->target->as<IdentifierExprAST>();
        targetName = target->name;

        // Check for `_` discard
        if (targetName.id == 0) {
            isDiscard = true;
        } else {
            // Verify the variable exists
            const ValueDeclAST* decl = ctx.lookupValue(targetName);
            if (!decl) {
                ctx.error(stmt->target, DiagCode::E2001,
                          "undefined variable '", ctx.pool.lookup(targetName), "'");
                return false;
            }

            // Verify it's a variable (not a function, enum, etc.)
            if (!decl->isa<VarDeclAST>()) {
                ctx.error(stmt->target, DiagCode::E3003,
                          "'", ctx.pool.lookup(targetName), "' is not a variable");
                return false;
            }
        }
    } else {
        // No target means `_` discard
        isDiscard = true;
    }

    // ─── 3. Resolve the call expression ─────────────────────────────────────
    if (!stmt->call) {
        ctx.error(stmt, DiagCode::E3003, "spawn statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExpr(stmt->call, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExpr
        return false;
    }

    // ─── 4. Store in pending list if not `_` ──────────────────────────────
    if (!isDiscard && targetName.id != 0) {
        ctx.addPendingSpawn(targetName, stmt->call, stmt->loc);
    }

    return false;  // spawn does not transfer control
}

// ─── resolveJoinStmt ───────────────────────────────────────────────────────

bool resolveJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.contexts.insideFunction()) {
        ctx.error(stmt, DiagCode::E3006,
                  "join statement outside of function body");
        return false;
    }

    // ─── 2. Check each target variable ─────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.error(target, DiagCode::E3003,
                      "join target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── 3. Check if this is a pending spawn operation ─────────────────
        if (ctx.hasPendingSpawn(targetName)) {
            // Resolve the spawn operation
            ctx.resolveSpawn(targetName);
        } else {
            ctx.error(target, DiagCode::E3003,
                      "'", ctx.pool.lookup(targetName), "' was not declared with spawn");
            return false;
        }

        // ─── 4. Verify the variable exists ─────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.error(target, DiagCode::E2001,
                      "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        // TODO: Change variable type from Future<T> to T
        // This would require updating the declaration's type
    }

    return false;  // join does not transfer control
}

} // namespace sema