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
/// @architectural_note RAII Guards
///   All scope and context management is done via RAII guards to ensure
///   proper cleanup even when errors occur.

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

bool resolveBlock(const BlockStmtAST* block, SemaContext& ctx) {
    if (!block) return false;

    // ─── RAII: Push block context ──────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::Block, 
                                   const_cast<BlockStmtAST*>(block), block->loc);

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

    // ─── RAII: Push a new scope for the block ──────────────────────────────
    SymbolScope scope(ctx);

    // ─── Resolve each statement ─────────────────────────────────────────
    for (const StmtAST* stmt : block->stmts) {
        if (transfers) {
            ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, stmt, "unreachable code");
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

    // ─── Pop pending narrowing level ──────────────────────────────────────
    if (hasAppliedPendingNarrowing) {
        ctx.stack.popNarrowingLevel();
    }

    // ─── SymbolScope and ScopedSemanticContext automatically pop ───────────

    return transfers;
}

// =============================================================================
// resolveIfStmt
// =============================================================================

bool resolveIfStmt(const IfStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push if context ────────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::IfStmt,
                                   const_cast<IfStmtAST*>(stmt), stmt->loc);
    ctx.stack.setHasElse(stmt->elseBranch != nullptr);

    // ─── RAII: ScopedIfCondition for narrowing detection ──────────────────
    ScopedIfCondition ifContext(ctx, stmt->elseBranch != nullptr);

    // ─── Resolve condition with target type = bool ─────────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);

    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Extract narrowing info from the condition ─────────────────────────
    NarrowingInfo info = extractNarrowingsFromCondition(stmt->condition, ctx);
    bool hasNarrowing = info.hasNarrowing;

    // ─── Resolve then branch ──────────────────────────────────────────────
    bool thenReturns = false;

    if (hasNarrowing && !info.isEquality) {
        ScopedNarrowing narrowing(ctx, info.narrowings, false);
        thenReturns = stmt->thenBranch ? resolveStmt(stmt->thenBranch, ctx) : false;
    } else {
        thenReturns = stmt->thenBranch ? resolveStmt(stmt->thenBranch, ctx) : false;
    }

    // ─── Resolve else branch ──────────────────────────────────────────────
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
            return true;
        }
    }

    // ─── Handle inverse narrowing for standalone if ───────────────────────
    if (!stmt->elseBranch && thenReturns && hasNarrowing && info.isEquality) {
        ctx.stack.setPendingInverseNarrowing(info);
    }

    return false;
}

// =============================================================================
// resolveSwitchStmt
// =============================================================================

bool resolveSwitchStmt(const SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Resolve subject expression ────────────────────────────────────────
    TypeAST* subjectType = resolveExpr(stmt->subject, ctx);
    if (!subjectType || subjectType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, stmt->subject,
                              "switch subject has unknown type");
        return false;
    }
    
    // ─── Validate subject type ─────────────────────────────────────────────
    if (!isValidSwitchType(subjectType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, stmt->subject,
                              "switch subject must be integer, enum, bool, char, or string");
        return false;
    }
    
    // ─── RAII: Push switch context ─────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::SwitchBody,
                                   const_cast<SwitchStmtAST*>(stmt), stmt->loc);
    
    // ─── Validate cases ─────────────────────────────────────────────────────
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
    
    // ─── Check exhaustiveness ──────────────────────────────────────────────
    if (!stmt->defaultBody && isEnumType(subjectType, ctx)) {
        switch_helpers::checkExhaustiveness(stmt, subjectType, ctx);
    }
    
    // ─── Resolve default clause ────────────────────────────────────────────
    if (stmt->defaultBody) {
        if (!resolveBlock(stmt->defaultBody, ctx)) {
            allCasesReturn = false;
        }
    }
    
    return allCasesReturn && (stmt->defaultBody || !isEnumType(subjectType, ctx));
}

// =============================================================================
// resolveSwitchCase
// =============================================================================

bool resolveSwitchCase(const SwitchCaseAST* switchCase, SemaContext& ctx) {
    if (!switchCase) return false;

    // ─── Validate each case value ──────────────────────────────────────────
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

    // ─── Resolve the case body ────────────────────────────────────────────
    if (switchCase->body) {
        return resolveBlock(switchCase->body, ctx);
    }

    return false;
}

// =============================================================================
// resolveForStmt
// =============================================================================

bool resolveForStmt(const ForStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push loop context ───────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::LoopBody,
                                   const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── RAII: Push a scope for loop variables ─────────────────────────────
    SymbolScope scope(ctx);

    // ─── Resolve AND REGISTER the index binding ──────────────────────────
    if (stmt->indexVar) {
        TypeAST* indexType = resolveType(stmt->indexVar->type, ctx);
        
        // ─── ALWAYS register the variable ──────────────────────────────────
        ctx.insertValue(stmt->indexVar);
        
        if (indexType && !isIntegerType(indexType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->indexVar,
                                  "index variable must be an integer type, got ",
                                  debug::typeToString(indexType, ctx.pool));
        }
    }

    // ─── Resolve AND REGISTER the value binding ──────────────────────────
    if (stmt->valueVar) {
        TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
        
        // ─── ALWAYS register the value variable ───────────────────────────
        ctx.insertValue(stmt->valueVar);
        
        // Value type validation will happen against the iterable
    }

    // ─── Resolve the iterable expression ──────────────────────────────────
    if (stmt->iterable) {
        TypeAST* iterableType = resolveExpr(stmt->iterable, ctx);
        if (!iterableType || iterableType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidIterator, stmt->iterable,
                                  "iterable has unknown type");
        } else {
            // ─── Validate value type against iterable element type ──────
            if (stmt->valueVar && iterableType->isa<ArrayTypeAST>()) {
                const ArrayTypeAST* arrayType = iterableType->as<ArrayTypeAST>();
                const TypeAST* elementType = arrayType->element;
                
                if (stmt->valueVar->type) {
                    TypeAST* valueType = resolveType(stmt->valueVar->type, ctx);
                    if (valueType && !typesEqual(valueType, elementType)) {
                        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->valueVar,
                                              "value type '", debug::typeToString(valueType, ctx.pool),
                                              "' does not match iterable element type '",
                                              debug::typeToString(elementType, ctx.pool), "'");
                    }
                }
            }
        }
    }

    // ─── Resolve the step expression (if present) ──────────────────────────
    if (stmt->step) {
        PrimitiveTypeAST* numericType = ctx.getIntType();
        TypeAST* stepType = resolveExprWithTarget(stmt->step, numericType, ctx);
        if (!stepType || stepType->isa<UnknownTypeAST>()) {
            // Error already reported - continue
        }
    }

    // ─── Resolve the loop body ─────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── SymbolScope and ScopedSemanticContext automatically pop ───────────

    return false;
}

// =============================================================================
// resolveWhileStmt
// =============================================================================

bool resolveWhileStmt(const WhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push loop context ───────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::LoopBody,
                                   const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── Resolve the condition against bool type ──────────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Resolve the loop body ─────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── ScopedSemanticContext automatically pops ─────────────────────────

    return false;
}

// =============================================================================
// resolveDoWhileStmt
// =============================================================================

bool resolveDoWhileStmt(const DoWhileStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── RAII: Push loop context ───────────────────────────────────────────
    ScopedSemanticContext context(ctx, ContextKind::LoopBody,
                                   const_cast<StmtAST*>(stmt->body), stmt->loc);

    // ─── Resolve the loop body ─────────────────────────────────────────────
    if (stmt->body) {
        resolveStmt(stmt->body, ctx);
    }

    // ─── Resolve the condition against bool type ──────────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(stmt->condition, boolType, ctx);
    
    if (!condType || condType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── ScopedSemanticContext automatically pops ─────────────────────────

    return false;
}

// =============================================================================
// resolveReturnStmt
// =============================================================================

bool resolveReturnStmt(const ReturnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBreak, stmt,
                              "return statement outside of function body");
        return true;
    }

    // ─── Get the current expected return type from the stack ──────────────
    const TypeAST* expectedType = ctx.stack.currentReturnType();

    // ─── Validate return value against expected type ───────────────────────
    if (stmt->value) {
        // ─── Non-void return ──────────────────────────────────────────────
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
        // ─── Void return (no value) ──────────────────────────────────────
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
static bool hasSideEffects(const ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case ASTKind::CallExpr:
            return true;

        case ASTKind::IntrinsicCallExpr: {
            const IntrinsicCallExprAST* intrinsic = expr->as<IntrinsicCallExprAST>();
            std::string nameStr = ctx.pool.lookup(intrinsic->intrinsicName);
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

        case ASTKind::AssignExpr:
            return true;

        case ASTKind::PipelineExpr: {
            const PipelineExprAST* pipeline = expr->as<PipelineExprAST>();
            if (hasSideEffects(pipeline->seed, ctx)) return true;
            for (const PipelineStepAST* step : pipeline->steps) {
                if (hasSideEffects(step->callable, ctx)) return true;
                for (const ExprAST* arg : step->packArgs) {
                    if (hasSideEffects(arg, ctx)) return true;
                }
            }
            return false;
        }

        case ASTKind::PipelineStep: {
            const PipelineStepAST* step = expr->as<PipelineStepAST>();
            if (hasSideEffects(step->callable, ctx)) return true;
            for (const ExprAST* arg : step->packArgs) {
                if (hasSideEffects(arg, ctx)) return true;
            }
            return false;
        }

        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            return hasSideEffects(bin->left, ctx) || hasSideEffects(bin->right, ctx);
        }

        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            return hasSideEffects(unary->operand, ctx);
        }

        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            return hasSideEffects(field->object, ctx);
        }

        case ASTKind::IndexExpr: {
            const IndexExprAST* index = expr->as<IndexExprAST>();
            return hasSideEffects(index->target, ctx) || 
                   hasSideEffects(index->index, ctx);
        }

        case ASTKind::SliceExpr: {
            const SliceExprAST* slice = expr->as<SliceExprAST>();
            if (hasSideEffects(slice->target, ctx)) return true;
            if (slice->start && hasSideEffects(slice->start, ctx)) return true;
            if (slice->end && hasSideEffects(slice->end, ctx)) return true;
            return false;
        }

        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* sl = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : sl->inits) {
                if (hasSideEffects(init->value, ctx)) return true;
            }
            return false;
        }

        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* al = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : al->elements) {
                if (hasSideEffects(elem, ctx)) return true;
            }
            return false;
        }

        case ASTKind::IfExpr: {
            const IfExprAST* ifExpr = expr->as<IfExprAST>();
            if (hasSideEffects(ifExpr->condition, ctx)) return true;
            if (hasSideEffects(ifExpr->thenBranch, ctx)) return true;
            if (hasSideEffects(ifExpr->elseBranch, ctx)) return true;
            return false;
        }

        case ASTKind::NullCoalesceExpr: {
            const NullCoalesceExprAST* nc = expr->as<NullCoalesceExprAST>();
            return hasSideEffects(nc->value, ctx) || 
                   hasSideEffects(nc->fallback, ctx);
        }

        case ASTKind::LiteralExpr:
        case ASTKind::IdentifierExpr:
        case ASTKind::ModuleAccessExpr:
        case ASTKind::RangeExpr:
            return false;

        default:
            return false;
    }
}

bool resolveExprStmt(const ExprStmtAST* stmt, SemaContext& ctx) {
    if (!stmt || !stmt->expr) return false;

    // ─── Resolve the expression ────────────────────────────────────────────
    TypeAST* exprType = resolveExpr(stmt->expr, ctx);
    if (!exprType || exprType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, stmt->expr,
                              "expression has unknown type");
        return false;
    }

    // ─── Check for discarded non-void value ──────────────────────────────
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

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement outside of function body");
        return false;
    }

    // ─── Check the target variable ─────────────────────────────────────────
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

    // ─── Check for `_` discard ─────────────────────────────────────────────
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

    // ─── Resolve the call expression ───────────────────────────────────────
    if (!stmt->call) {
        ctx.diagnostics.error(DiagCode::Sem_AsyncOutsideFunction, stmt,
                              "async statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExpr(stmt->call, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Store in pending list if not `_` ──────────────────────────────────
    if (targetName.id != 0) {
        ctx.addPendingAsync(targetName, stmt->call, stmt->loc);
    }

    return false;
}

// ─── resolveAwaitStmt ──────────────────────────────────────────────────────

bool resolveAwaitStmt(const AwaitStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_AwaitOutsideFunction, stmt,
                              "await statement outside of function body");
        return false;
    }

    // ─── Check each target variable ────────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "await target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── Check if this is a pending async operation ────────────────────
        if (ctx.hasPendingAsync(targetName)) {
            ctx.resolveAsync(targetName);
        } else if (ctx.hasPendingSpawn(targetName)) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "'", ctx.pool.lookup(targetName), "' was declared with spawn, not async. Use 'join' instead.");
            return false;
        } else {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, target,
                                  "'", ctx.pool.lookup(targetName), "' is not a pending async operation");
            return false;
        }

        // ─── Verify the variable exists ────────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }
    }

    return false;
}

// ─── resolveSpawnStmt ──────────────────────────────────────────────────────

bool resolveSpawnStmt(const SpawnStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                              "spawn statement outside of function body");
        return false;
    }

    // ─── Check the target variable ─────────────────────────────────────────
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

    // ─── Resolve the call expression ───────────────────────────────────────
    if (!stmt->call) {
        ctx.diagnostics.error(DiagCode::Sem_SpawnOutsideFunction, stmt,
                              "spawn statement requires a call expression");
        return false;
    }

    TypeAST* callType = resolveExpr(stmt->call, ctx);
    if (!callType || callType->isa<UnknownTypeAST>()) {
        return false;
    }

    // ─── Store in pending list if not `_` ──────────────────────────────────
    if (!isDiscard && targetName.id != 0) {
        ctx.addPendingSpawn(targetName, stmt->call, stmt->loc);
    }

    return false;
}

// ─── resolveJoinStmt ───────────────────────────────────────────────────────

bool resolveJoinStmt(const JoinStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return false;

    // ─── Check: Must be inside a function body ─────────────────────────────
    if (!ctx.stack.insideFunction()) {
        ctx.diagnostics.error(DiagCode::Sem_JoinOutsideFunction, stmt,
                              "join statement outside of function body");
        return false;
    }

    // ─── Check each target variable ────────────────────────────────────────
    for (const ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "join target must be a variable (not an expression)");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        InternedString targetName = id->name;

        // ─── Check if this is a pending spawn operation ────────────────────
        if (ctx.hasPendingSpawn(targetName)) {
            ctx.resolveSpawn(targetName);
        } else if (ctx.hasPendingAsync(targetName)) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "'", ctx.pool.lookup(targetName), "' was declared with async, not spawn. Use 'await' instead.");
            return false;
        } else {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, target,
                                  "'", ctx.pool.lookup(targetName), "' is not a pending spawn operation");
            return false;
        }

        // ─── Verify the variable exists ────────────────────────────────────
        const ValueDeclAST* decl = ctx.lookupValue(targetName);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, target,
                                  "undefined variable '", ctx.pool.lookup(targetName), "'");
            return false;
        }
    }

    return false;
}

} // namespace sema