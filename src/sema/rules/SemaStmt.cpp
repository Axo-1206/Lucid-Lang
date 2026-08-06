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
#include "core/diagnostics/Diagnostic.hpp"
#include "debug/DebugUtils.hpp"

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
    ctx.stack.pushBlock(const_cast<BlockStmtAST*>(block), block->loc);

    bool transfers = false;
    bool hasAppliedPendingNarrowing = false;

    // Apply pending inverse narrowing
    if (ctx.stack.hasPendingInverseNarrowing()) {
        const NarrowingInfo& pendingInfo = ctx.stack.getPendingInverseNarrowing();
        if (pendingInfo.hasNarrowing) {
            ctx.stack.pushNarrowingLevel(true);
            for (const auto& [varName, narrowedType] : pendingInfo.narrowings) {
                ctx.stack.narrowVariable(varName, narrowedType);
            }
            ctx.stack.clearPendingInverseNarrowing();
            hasAppliedPendingNarrowing = true;
        }
    }

    // Push a new scope for the block (for local variables)
    ctx.pushScope();

    // ─── Resolve each statement ─────────────────────────────────────────
    for (const StmtAST* stmt : block->stmts) {
        if (transfers) {
            ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt, "unreachedable code");
            continue;
        }
        transfers = resolveStmt(stmt, ctx);
        if (transfers) {
            break;
        }
    }

    // Check for unresolved async/spawn operations
    for (const InternedString& name : ctx.getPendingAsyncNames()) {
        ctx.diagnostics.warning(DiagCode::Warn_UnawaitedAsync, block,
                                "async '", ctx.pool.lookup(name), "' was never awaited");
    }

    for (const InternedString& name : ctx.getPendingSpawnNames()) {
        ctx.diagnostics.warning(DiagCode::Warn_UnjoinedSpawn, block,
                                "spawn '", ctx.pool.lookup(name), "' was never joined");
    }

    // Pop the block scope - local variables are no longer visible
    ctx.popScope();

    // Pop pending narrowing level
    if (hasAppliedPendingNarrowing) {
        ctx.stack.popNarrowingLevel();
    }

    ctx.stack.pop();

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
bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Push if context for type narrowing ──────────────────────────
    ctx.stack.push(ContextKind::IfStmt, const_cast<IfStmtAST*>(stmt), stmt->loc);
    ctx.stack.setHasElse(stmt->elseBranch != nullptr);

    // ─── 2. Resolve condition with target type = bool ────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();

    ctx.stack.setIfConditionCtx(true);
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    ctx.stack.setIfConditionCtx(false);

    if (!condType || condType->isa<UnknownTypeAST>()) {
        ctx.stack.pop();
        return false;
    }

    // ─── 3. Extract narrowing info from the condition ────────────────────
    NarrowingInfo info = extractNarrowingsFromCondition(stmt->condition, ctx);
    bool hasNarrowing = info.hasNarrowing;

    // ─── 4. Resolve then branch ──────────────────────────────────────────
    bool thenReturns = false;

    if (hasNarrowing && !info.isEquality) {
        ScopedNarrowing narrowing(ctx, info.narrowings, false);
        thenReturns = stmt->thenBranch ? resolveStmt(stmt->thenBranch, ctx) : false;
    } else {
        thenReturns = stmt->thenBranch ? resolveStmt(stmt->thenBranch, ctx) : false;
    }

    // ─── 5. Resolve else branch ──────────────────────────────────────────
    if (stmt->elseBranch) {
        bool elseReturns = false;

        if (stmt->elseBranch->isa<IfStmtAST>()) {
            elseReturns = resolveIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else {
            if (hasNarrowing && info.isEquality) {
                ScopedNarrowing narrowing(ctx, info.narrowings, true);
                elseReturns = resolveStmt(stmt->elseBranch, ctx);
            } else {
                elseReturns = resolveStmt(stmt->elseBranch, ctx);
            }
        }

        if (thenReturns && elseReturns) {
            ctx.stack.pop();
            return true;
        }
    }

    // ─── 6. Handle inverse narrowing for standalone if ───────────────────
    if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
        ctx.stack.setPendingInverseNarrowing(info);
    }

    ctx.stack.pop();
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
    // ─── 1. Resolve subject expression ──────────────────────────────
    TypeAST* subjectType = resolveExpr(stmt->subject, ctx);
    if (!subjectType || subjectType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, stmt->subject,
                              "switch subject has unknown type");
        ctx.stack.push(ContextKind::SwitchBody, const_cast<SwitchStmtAST*>(stmt), stmt->loc);
        ctx.stack.pop();
        return false;
    }
    
    // ─── 2. Validate subject type ──────────────────────────────────────
    if (!isValidSwitchType(subjectType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, stmt->subject,
                              "switch subject must be integer, enum, bool, char, or string");
        return false;
    }
    
    // ─── 3. Push switch context ──────────────────────────────────────
    ctx.stack.push(ContextKind::SwitchBody, const_cast<SwitchStmtAST*>(stmt), stmt->loc);
    
    // ─── 4. Validate cases ─────────────────────────────────────────────
    bool allCasesReturn = true;
    
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        // Validate each case value against the subject type
        for (ExprAST* value : caseStmt->values) {
            TypeAST* valueType = resolveExprWithTarget(value, subjectType, ctx);
            if (!valueType || valueType->isa<UnknownTypeAST>()) {
                continue;
            }
            
            if (!isSwitchCaseCompatible(value, subjectType, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, value,
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
    
    ctx.stack.pop();
    
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
            ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, value,
                                  "case value must be a literal, enum variant, or range");
            continue;
        }

        TypeAST* valueType = resolveExpr(value, ctx);
        if (!valueType || valueType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, value,
                                  "case value has unknown type");
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
    ctx.stack.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Push a scope for loop variables ──────────────────────────────
    ctx.pushScope();

    // ─── 3. Resolve AND REGISTER the index binding ──────────────────────
    if (stmt->indexVar) {
        TypeAST* indexType = resolveType(stmt->indexVar->type, ctx);
        if (indexType) {
            ctx.insertValue(stmt->indexVar);
        }
        if (indexType && !isIntegerType(indexType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->indexVar,
                                  "index variable must be an integer type");
        }
    }

    // ─── 4. Resolve AND REGISTER the value binding ──────────────────────
    if (stmt->valueVar) {
        TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
        if (valueType) {
            ctx.insertValue(stmt->valueVar);
        }
    }

    // ─── 5. Resolve the iterable expression ──────────────────────────────
    if (stmt->iterable) {
        TypeAST* iterableType = resolveExpr(stmt->iterable, ctx);
        if (!iterableType || iterableType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidIterator, stmt->iterable,
                                  "iterable has unknown type");
            ctx.popScope();
            ctx.stack.pop();
            return false;
        }
    }

    // ─── 6. Resolve the step expression (if present) ──────────────────────
    if (stmt->step) {
        PrimitiveTypeAST* numericType = ctx.getIntType();
        TypeAST* stepType = resolveExprWithTarget(stmt->step, numericType, ctx);
        if (!stepType || stepType->isa<UnknownTypeAST>()) {
            ctx.popScope();
            ctx.stack.pop();
            return false;
        }
    }

    // ─── 7. Resolve the loop body ─────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── 8. Pop the scope ──────────────────────────────────────────────────
    ctx.popScope();

    // ─── 9. Pop loop context ──────────────────────────────────────────────
    ctx.stack.pop();

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
    ctx.stack.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Resolve the condition against bool type ──────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        ctx.stack.pop();
        return false;
    }

    // ─── 3. Resolve the loop body ─────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── 4. Pop loop context ──────────────────────────────────────────────
    ctx.stack.pop();

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
    ctx.stack.pushLoop(const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── 2. Resolve the loop body ─────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── 3. Resolve the condition against bool type ──────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        ctx.stack.pop();
        return false;
    }

    // ─── 4. Pop loop context ──────────────────────────────────────────────
    ctx.stack.pop();

    return false;
}

// =============================================================================
// resolveReturnStmt
// =============================================================================

/// @brief Resolve a return statement.
///
/// The return statement exits the enclosing function.
/// Uses the simplified ReturnStack for type validation.
///
/// @param stmt The return statement.
/// @param ctx The semantic context.
/// @return true (return always transfers control out of the block).
bool resolveReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBreak, stmt,
                              "return statement outside of function body");
        return true;
    }

    // ─── 2. Get the current expected return type from the stack ───────────
    const TypeAST* expectedType = ctx.stack.currentReturnType();

    // ─── 3. Validate return value against expected type ────────────────────
    if (stmt->value) {
        // ─── 3a. Non-void return ──────────────────────────────────────────
        if (!expectedType) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, stmt,
                                  "return value provided but function has no return type (expected void)");
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
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, stmt->value,
                                      "cannot return err to non-fallible return type");
                return true;
            }
        }

        if (stmt->value->valueState == ValueState::Nil) {
            if (!isNullableType(expectedType)) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, stmt->value,
                                      "cannot return nil to non-nullable return type");
                return true;
            }
        }

    } else {
        // ─── 3b. Void return (no value) ────────────────────────────────────
        if (expectedType) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, stmt,
                                  "void return statement but function expects a return value (", 
                                  debug::typeToString(expectedType, ctx.pool), ")");
            return true;
        }
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

    if (!ctx.stack.insideLoop() && !ctx.stack.insideSwitch()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBreak, stmt,
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

    if (!ctx.stack.insideLoop()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidContinue, stmt,
                              "continue statement outside of loop");
        return true;
    }

    return true;
}

// =============================================================================
// resolveExprStmt
// =============================================================================

/// @brief Check if an expression has side effects.
///
/// An expression has side effects if it:
///   - Is a function call (call_expr)
///   - Is an assignment (assign_expr)
///   - Contains a function call or assignment in any sub-expression
///   - Is a pipeline expression (may contain calls)
///   - Is a field access that might be a function call (though Lucid has no methods)
///
/// @param expr The expression to check.
/// @param ctx The semantic context.
/// @return true if the expression has side effects, false otherwise.
static bool hasSideEffects(const ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        // ─── Call expressions always have side effects ──────────────────────
        case ASTKind::CallExpr:
            return true;

        // ─── Intrinsic calls may have side effects ──────────────────────────
        case ASTKind::IntrinsicCallExpr: {
            const IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
            // Memory operations and FFI intrinsics have side effects
            InternedString name = intrinsic->intrinsicName;
            std::string nameStr = ctx.pool.lookup(name);
            if (nameStr == "memcpy" || nameStr == "memmove" || nameStr == "memset" ||
                nameStr == "alloc" || nameStr == "free" ||
                nameStr == "arena_create" || nameStr == "arena_alloc" || 
                nameStr == "arena_free" || nameStr == "arena_reset" ||
                nameStr == "atomic_store" || nameStr == "atomic_add" ||
                nameStr == "atomic_sub" || nameStr == "atomic_and" ||
                nameStr == "atomic_or" || nameStr == "atomic_xor" ||
                nameStr == "atomic_cas") {
                return true;
            }
            return false;
        }

        // ─── Assignment always has side effects ─────────────────────────────
        case ASTKind::AssignExpr:
            return true;

        // ─── Pipeline may contain calls in steps ────────────────────────────
        case ASTKind::PipelineExpr: {
            const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            // Check seed
            if (hasSideEffects(pipeline->seed, ctx)) return true;
            // Check each step
            for (const PipelineStepAST* step : pipeline->steps) {
                if (hasSideEffects(step->callable, ctx)) return true;
                for (const ExprAST* arg : step->packArgs) {
                    if (hasSideEffects(arg, ctx)) return true;
                }
            }
            return false;
        }

        // ─── Pipeline step (shouldn't appear standalone, but handle it) ─────
        case ASTKind::PipelineStep: {
            const PipelineStepAST* step = expr->as<PipelineStepAST>();
            if (hasSideEffects(step->callable, ctx)) return true;
            for (const ExprAST* arg : step->packArgs) {
                if (hasSideEffects(arg, ctx)) return true;
            }
            return false;
        }

        // ─── Binary expressions: check both operands ────────────────────────
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            return hasSideEffects(bin->left, ctx) || hasSideEffects(bin->right, ctx);
        }

        // ─── Unary expressions: check operand ──────────────────────────────
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            return hasSideEffects(unary->operand, ctx);
        }

        // ─── Field access: check object ─────────────────────────────────────
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            return hasSideEffects(field->object, ctx);
        }

        // ─── Index expression: check target and index ──────────────────────
        case ASTKind::IndexExpr: {
            const IndexExprAST* index = expr->as<IndexExprAST>();
            return hasSideEffects(index->target, ctx) || 
                   hasSideEffects(index->index, ctx);
        }

        // ─── Slice expression: check target and bounds ─────────────────────
        case ASTKind::SliceExpr: {
            const SliceExprAST* slice = expr->as<SliceExprAST>();
            if (hasSideEffects(slice->target, ctx)) return true;
            if (slice->start && hasSideEffects(slice->start, ctx)) return true;
            if (slice->end && hasSideEffects(slice->end, ctx)) return true;
            return false;
        }

        // ─── Struct literal: check each field initializer ──────────────────
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* sl = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : sl->inits) {
                if (hasSideEffects(init->value, ctx)) return true;
            }
            return false;
        }

        // ─── Array literal: check each element ─────────────────────────────
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* al = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : al->elements) {
                if (hasSideEffects(elem, ctx)) return true;
            }
            return false;
        }

        // ─── If expression: check condition and branches ────────────────────
        case ASTKind::IfExpr: {
            const IfExprAST* ifExpr = expr->as<IfExprAST>();
            if (hasSideEffects(ifExpr->condition, ctx)) return true;
            if (hasSideEffects(ifExpr->thenBranch, ctx)) return true;
            if (hasSideEffects(ifExpr->elseBranch, ctx)) return true;
            return false;
        }

        // ─── Null coalesce: check both sides ───────────────────────────────
        case ASTKind::NullCoalesceExpr: {
            const NullCoalesceExprAST* nc = expr->as<NullCoalesceExprAST>();
            return hasSideEffects(nc->value, ctx) || 
                   hasSideEffects(nc->fallback, ctx);
        }

        // ─── Literals and identifiers have no side effects ──────────────────
        case ASTKind::LiteralExpr:
        case ASTKind::IdentifierExpr:
        case ASTKind::ModuleAccessExpr:
        case ASTKind::RangeExpr:
            return false;

        // ─── Default: assume no side effects for unknown kinds ─────────────
        default:
            return false;
    }
}

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

    // ─── 1. Resolve the expression ──────────────────────────────────────
    TypeAST* exprType = resolveExpr(stmt->expr, ctx);
    if (!exprType || exprType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, stmt->expr,
                              "expression has unknown type");
        return false;
    }

    // ─── 2. Check for discarded non-void value ─────────────────────────────
    // If the expression has a non-void type and no side effects, warn
    if (exprType && !exprType->isa<UnknownTypeAST>()) {
        if (!hasSideEffects(stmt->expr, ctx)) {
            ctx.diagnostics.warning(DiagCode::Warn_DiscardedResult, stmt,
                                    "expression result is discarded (no side effects)");
        }
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
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement outside of function body");
        return false;
    }

    // ─── 2. Check the target variable ──────────────────────────────────────
    if (!stmt->target) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement requires a target variable");
        return false;
    }

    if (!stmt->target->isa<IdentifierExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt->target,
                              "async target must be a variable (not an expression)");
        return false;
    }

    const IdentifierExprAST* target = stmt->target->as<IdentifierExprAST>();
    InternedString targetName = target->name;

    // ─── 3. Check for `_` discard ──────────────────────────────────────────
    if (targetName.id != 0) {  // Not `_`
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, stmt->target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        if (!decl->isa<VarDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->target,
                                  "'", ctx.pool.lookup(targetName), "' is not a variable");
            return false;
        }
    }

    // ─── 4. Resolve the call expression ─────────────────────────────────────
    if (!stmt->call) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExpr(stmt->call, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── 5. Store in pending list if not `_` ──────────────────────────────
    if (targetName.id != 0) {
        ctx.addPendingAsync(targetName, stmt->call, stmt->loc);
    }

    return false;
}

// ─── resolveAwaitStmt ──────────────────────────────────────────────────────

bool resolveAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AwaitOutsideFunction, stmt,
                              "await statement outside of function body");
        return false;
    }

    // ─── 2. Check each target variable ─────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "await target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── 3. Check if this is a pending async operation ────────────────
        if (ctx.hasPendingAsync(targetName)) {
            ctx.resolveAsync(targetName);
        } else {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "'", ctx.pool.lookup(targetName), "' was not declared with async");
            return false;
        }

        // ─── 4. Verify the variable exists ─────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        // TODO: Change variable type from Future<T> to T
    }

    return false;
}

// ─── resolveSpawnStmt ──────────────────────────────────────────────────────

bool resolveSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                              "spawn statement outside of function body");
        return false;
    }

    // ─── 2. Check the target variable ──────────────────────────────────────
    InternedString targetName;
    bool isDiscard = false;

    if (stmt->target) {
        if (!stmt->target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt->target,
                                  "spawn target must be a variable (not an expression)");
            return false;
        }

        const IdentifierExprAST* target = stmt->target->as<IdentifierExprAST>();
        targetName = target->name;

        if (targetName.id == 0) {
            isDiscard = true;
        } else {
            const ValueDeclAST* decl = ctx.lookupValue(targetName);
            if (!decl) {
                ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, stmt->target,
                                      "undefined variable '", ctx.pool.lookup(targetName), "'");
                return false;
            }

            if (!decl->isa<VarDeclAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->target,
                                      "'", ctx.pool.lookup(targetName), "' is not a variable");
                return false;
            }
        }
    } else {
        isDiscard = true;
    }

    // ─── 3. Resolve the call expression ─────────────────────────────────────
    if (!stmt->call) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                              "spawn statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExpr(stmt->call, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── 4. Store in pending list if not `_` ──────────────────────────────
    if (!isDiscard && targetName.id != 0) {
        ctx.addPendingSpawn(targetName, stmt->call, stmt->loc);
    }

    return false;
}

// ─── resolveJoinStmt ───────────────────────────────────────────────────────

bool resolveJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── 1. Check: Must be inside a function body ──────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_JoinOutsideFunction, stmt,
                              "join statement outside of function body");
        return false;
    }

    // ─── 2. Check each target variable ─────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "join target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── 3. Check if this is a pending spawn operation ─────────────────
        if (ctx.hasPendingSpawn(targetName)) {
            ctx.resolveSpawn(targetName);
        } else {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "'", ctx.pool.lookup(targetName), "' was not declared with spawn");
            return false;
        }

        // ─── 4. Verify the variable exists ─────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }

        // TODO: Change variable type from Future<T> to T
    }

    return false;
}

} // namespace sema