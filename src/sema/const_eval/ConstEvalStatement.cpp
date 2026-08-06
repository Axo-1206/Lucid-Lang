/// @file const_eval/ConstEvalStatement.cpp
/// @brief Statement execution for const functions.

#include "ConstEvalHelpers.hpp"
#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"
#include "sema/Sema.hpp"

namespace sema {

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeStmt(SemaContext& ctx, const StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            return executeBlock(ctx, stmt->as<BlockStmtAST>());
        case ASTKind::ReturnStmt:
            return executeReturn(ctx, stmt->as<ReturnStmtAST>());
        case ASTKind::IfStmt:
            return executeIf(ctx, stmt->as<IfStmtAST>());
        case ASTKind::WhileStmt:
            return executeWhile(ctx, stmt->as<WhileStmtAST>());
        case ASTKind::ExprStmt:
            return executeExprStmt(ctx, stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:
            return executeDeclStmt(ctx, stmt->as<DeclStmtAST>());
        default:
            return ConstantValue::unknown();
    }
}

ConstantValue ConstEvaluator::executeBlock(SemaContext& ctx, const BlockStmtAST* block) {
    if (!block) return ConstantValue::voidValue();

    ctx.pushScope();
    ConstantValue result = ConstantValue::voidValue();

    for (const StmtPtr stmt : block->stmts) {
        result = executeStmt(ctx, stmt);
        if (result.isError()) break;
        if (result.isUnknown()) break;
    }

    ctx.popScope();
    return result;
}

ConstantValue ConstEvaluator::executeReturn(SemaContext& ctx, const ReturnStmtAST* stmt) {
    if (stmt->value) {
        ConstantValue result = evaluate(ctx, stmt->value);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        return result;
    }
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeIf(SemaContext& ctx, const IfStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // ─── 1. Push if context for type narrowing ──────────────────────────
    ScopedIfCondition ifContext(ctx, stmt->elseBranch != nullptr);

    // ─── 2. Evaluate condition ───────────────────────────────────────────
    ConstantValue cond = evaluate(ctx, stmt->condition);
    if (cond.isError()) return cond;
    if (cond.isUnknown()) return ConstantValue::unknown();

    if (!cond.isBool()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                              "if condition must be bool");
        return ConstantValue::error();
    }

    // ─── 3. Get narrowing info detected during condition evaluation ────
    NarrowingInfo info = ctx.stack.getPendingNarrowing();
    ctx.stack.clearPendingNarrowing();

    // ─── 4. Execute the appropriate branch ──────────────────────────────
    if (cond.asBool()) {
        // ─── Then branch ──────────────────────────────────────────────────
        if (stmt->thenBranch) {
            // Apply normal narrowing for inequality conditions (x != nil)
            // When x != nil is true, x is non-nullable
            if (info.hasNarrowing && !info.isEquality) {
                ScopedNarrowing narrowing(ctx, info.narrowings, false);
                return executeStmt(ctx, stmt->thenBranch);
            }
            return executeStmt(ctx, stmt->thenBranch);
        }
    } else {
        // ─── Else branch ──────────────────────────────────────────────────
        if (stmt->elseBranch) {
            // Apply inverse narrowing for equality conditions (x == nil)
            // When x == nil is false, x is definitely non-nullable
            if (info.hasNarrowing && info.isEquality) {
                ScopedNarrowing narrowing(ctx, info.narrowings, true);
                return executeStmt(ctx, stmt->elseBranch);
            }
            return executeStmt(ctx, stmt->elseBranch);
        }
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeWhile(SemaContext& ctx, const WhileStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    const size_t MAX_ITERATIONS = 10000;
    size_t iterations = 0;

    while (true) {
        if (++iterations > MAX_ITERATIONS) {
            ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, stmt,
                                  "while loop exceeded maximum iterations (",
                                  MAX_ITERATIONS, ")");
            return ConstantValue::error();
        }

        ConstantValue cond = evaluate(ctx, stmt->condition);
        if (cond.isError()) return cond;
        if (cond.isUnknown()) return ConstantValue::unknown();

        if (!cond.isBool()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, stmt->condition,
                                  "while condition must be bool");
            return ConstantValue::error();
        }

        if (!cond.asBool()) break;

        ConstantValue result = executeStmt(ctx, stmt->body);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        if (result.isVoid()) continue;
    }

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeExprStmt(SemaContext& ctx, const ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    ConstantValue result = evaluate(ctx, stmt->expr);
    if (result.isError()) return result;
    if (result.isUnknown()) return ConstantValue::unknown();

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeDeclStmt(SemaContext& ctx, const DeclStmtAST* stmt) {
    if (!stmt || !stmt->decl) return ConstantValue::voidValue();

    if (stmt->decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = stmt->decl->as<VarDeclAST>();
        if (var->keyword == DeclKeyword::Const && var->init) {
            ConstantValue val = evaluate(ctx, var->init);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            
            const_cast<ExprAST*>(var->init)->isConst = true;
            const_cast<ExprAST*>(var->init)->constValue = val;
            ctx.insertValue(var);
            return ConstantValue::voidValue();
        }
        
        ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->decl,
                              "mutable local variables not allowed in const functions");
        return ConstantValue::error();
    }

    return ConstantValue::unknown();
}

} // namespace sema