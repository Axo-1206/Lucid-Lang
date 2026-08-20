/// @file const_eval/ConstEvalStatement.cpp
/// @brief Statement execution for const functions.

#include "ConstEvalHelpers.hpp"
#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"
#include "sema/Sema.hpp"

namespace sema {

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeStmt(SemaContext& ctx, StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    switch (stmt->kind) {
        case ASTKind::BlockStmt:     return executeBlock(ctx, stmt->as<BlockStmtAST>());
        case ASTKind::ReturnStmt:    return executeReturn(ctx, stmt->as<ReturnStmtAST>());
        case ASTKind::IfStmt:        return executeIf(ctx, stmt->as<IfStmtAST>());
        case ASTKind::WhileStmt:     return executeWhile(ctx, stmt->as<WhileStmtAST>());
        case ASTKind::ForStmt:       return executeFor(ctx, stmt->as<ForStmtAST>());
        case ASTKind::SwitchStmt:    return executeSwitch(ctx, stmt->as<SwitchStmtAST>());
        case ASTKind::ExprStmt:      return executeExprStmt(ctx, stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:      return executeDeclStmt(ctx, stmt->as<DeclStmtAST>());
        default:                     return ConstantValue::unknown();
    }
}

ConstantValue ConstEvaluator::executeBlock(SemaContext& ctx, BlockStmtAST* block) {
    if (!block) return ConstantValue::voidValue();

    ctx.pushScope();
    ConstantValue result = ConstantValue::voidValue();

    for (StmtAST* stmt : block->stmts) {
        result = executeStmt(ctx, stmt);
        if (result.isError()) break;
        if (result.isUnknown()) break;
    }

    ctx.popScope();
    return result;
}

ConstantValue ConstEvaluator::executeReturn(SemaContext& ctx, ReturnStmtAST* stmt) {
    if (stmt->value) {
        ConstantValue result = evaluate(ctx, stmt->value);
        if (result.isError()) return result;
        if (result.isUnknown()) return ConstantValue::unknown();
        return result;
    }
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeIf(SemaContext& ctx, IfStmtAST* stmt) {
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

ConstantValue ConstEvaluator::executeWhile(SemaContext& ctx, WhileStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    const size_t MAX_ITERATIONS = 10000;
    size_t iterations = 0;

    while (true) {
        if (++iterations > MAX_ITERATIONS) {
            return ConstantValue::unknown();
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

ConstantValue ConstEvaluator::executeFor(SemaContext& ctx, ForStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    if (stmt->iterable && stmt->iterable->isa<RangeExprAST>()) {
        RangeExprAST* range = stmt->iterable->as<RangeExprAST>();
        
        auto loOpt = evaluateAsInt(ctx, range->lo);
        auto hiOpt = evaluateAsInt(ctx, range->hi);
        
        if (loOpt.has_value() && hiOpt.has_value()) {
            int64_t lo = loOpt.value();
            int64_t hi = hiOpt.value();
            bool isInclusive = !range->isExclusive;
            
            // Validation already done by resolveForStmt - just execute
            // If invalid, return error (shouldn't happen)
            if ((isInclusive && lo > hi) || (!isInclusive && lo >= hi)) {
                return ConstantValue::error();
            }
            
            int64_t step = 1;
            if (stmt->step) {
                auto stepOpt = evaluateAsInt(ctx, stmt->step);
                if (!stepOpt.has_value()) return ConstantValue::unknown();
                step = stepOpt.value();
                if (step <= 0) return ConstantValue::error();
            }
            
            size_t iterations = 0;
            for (int64_t i = lo; isInclusive ? i <= hi : i < hi; i += step) {
                if (++iterations > MAX_ITERATIONS) return ConstantValue::unknown();
                
                // Bind index variable for the body
                if (stmt->indexVar) {
                    stmt->indexVar->type = ctx.getIntType();
                }
                
                ConstantValue result = executeStmt(ctx, stmt->body);
                if (result.isError()) return result;
                if (result.isUnknown()) return ConstantValue::unknown();
                if (result.isVoid()) continue;
                return result;
            }
            return ConstantValue::voidValue();
        }
    }

    // Can't evaluate - fall back
    if (stmt->body) {
        executeStmt(ctx, stmt->body);
    }
    return ConstantValue::unknown();
}

ConstantValue ConstEvaluator::executeSwitch(SemaContext& ctx, SwitchStmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();

    // ─── Evaluate subject ──────────────────────────────────────────────────
    ConstantValue subjectVal = evaluate(ctx, stmt->subject);
    if (subjectVal.isError()) return subjectVal;
    if (subjectVal.isUnknown()) {
        // Can't evaluate - fall back to executing all cases for side effects
        for (const SwitchCaseAST* caseStmt : stmt->cases) {
            if (caseStmt->body) executeStmt(ctx, caseStmt->body);
        }
        if (stmt->defaultBody) executeStmt(ctx, stmt->defaultBody);
        return ConstantValue::unknown();
    }

    // ─── Try to match a case ──────────────────────────────────────────────
    for (const SwitchCaseAST* caseStmt : stmt->cases) {
        for (ExprAST* value : caseStmt->values) {
            bool matches = false;
            
            if (value->isa<RangeExprAST>()) {
                // Range case
                RangeExprAST* range = value->as<RangeExprAST>();
                auto loOpt = evaluateAsInt(ctx, range->lo);
                auto hiOpt = evaluateAsInt(ctx, range->hi);
                if (loOpt.has_value() && hiOpt.has_value() && subjectVal.isInt()) {
                    int64_t subj = subjectVal.asInt();
                    bool isInclusive = !range->isExclusive;
                    matches = isInclusive ? (subj >= loOpt.value() && subj <= hiOpt.value())
                                          : (subj >= loOpt.value() && subj < hiOpt.value());
                }
            } else {
                // Regular case
                ConstantValue caseVal = evaluate(ctx, value);
                if (caseVal.isError()) return caseVal;
                if (caseVal.isUnknown()) {
                    // Can't evaluate - fall back
                    for (const SwitchCaseAST* c : stmt->cases) {
                        if (c->body) executeStmt(ctx, c->body);
                    }
                    if (stmt->defaultBody) executeStmt(ctx, stmt->defaultBody);
                    return ConstantValue::unknown();
                }
                matches = compareEqual(ctx, subjectVal, caseVal);
            }
            
            if (matches) {
                if (caseStmt->body) {
                    return executeStmt(ctx, caseStmt->body);
                }
                return ConstantValue::voidValue();
            }
        }
    }

    // ─── No match ──────────────────────────────────────────────────────────
    if (stmt->defaultBody) {
        return executeStmt(ctx, stmt->defaultBody);
    }
    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeExprStmt(SemaContext& ctx, ExprStmtAST* stmt) {
    if (!stmt || !stmt->expr) return ConstantValue::voidValue();

    ConstantValue result = evaluate(ctx, stmt->expr);
    if (result.isError()) return result;
    if (result.isUnknown()) return ConstantValue::unknown();

    return ConstantValue::voidValue();
}

ConstantValue ConstEvaluator::executeDeclStmt(SemaContext& ctx, DeclStmtAST* stmt) {
    if (!stmt || !stmt->decl) return ConstantValue::voidValue();

    if (stmt->decl->isa<VarDeclAST>()) {
        VarDeclAST* var = stmt->decl->as<VarDeclAST>();
        if (var->keyword == DeclKeyword::Const && var->init) {
            ConstantValue val = evaluate(ctx, var->init);
            if (val.isError()) return val;
            if (val.isUnknown()) return ConstantValue::unknown();
            
            m_evalCache[var->init] = val;  // Cache the value
            var->init->isConst = true;     // Mark as const
            ctx.insertValue(var);
            return ConstantValue::voidValue();
        }
        
        ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, stmt->decl,
                              "mutable local variables not allowed in const functions");
        return ConstantValue::error();
    }

    return ConstantValue::unknown();
}
ConstantValue ConstEvaluator::executeFunction(SemaContext& ctx, FuncDeclAST* func,
                                               const std::vector<ConstantValue>& args) {
    if (!func) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                              "null function");
        return ConstantValue::error();
    }

    // ─── 0. Recursion depth guard ────────────────────────────────────────
    // evaluate()'s own MAX_RECURSION check only fires if m_recursionDepth
    // is actually incremented somewhere on the path it guards. Previously
    // nothing incremented it anywhere in the evalCall → executeFunction →
    // executeStmt → evaluate → evalCall cycle — only evaluateDecl did, a
    // completely different call path (VarDeclAST circular-dependency
    // detection, not function-call recursion). A recursive const function
    // had no depth limit at all: it would recurse via genuine C++ call
    // stack frames until the *compiler process itself* stack-overflowed.
    // EvaluationGuard/m_evaluating isn't a substitute either — that only
    // guards VarDeclAST cycles, never touched here.
    if (m_recursionDepth >= MAX_RECURSION) {
        ctx.diagnostics.error(DiagCode::Sem_CircularDependency, func,
                              "const function '", ctx.pool.lookup(func->name),
                              "' exceeded maximum recursion depth (",
                              MAX_RECURSION, ")");
        return ConstantValue::error();
    }
    m_recursionDepth++;
    struct DepthGuard {
        size_t& depth;
        ~DepthGuard() { depth--; }
    } depthGuard{m_recursionDepth};

    // ─── 1. Setup function context ──────────────────────────────────────
    ConstFunctionContext context(ctx, func);

    // ─── 2. Bind arguments to parameters ────────────────────────────────
    size_t argIndex = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (argIndex < args.size()) {
                // Create a synthetic literal for the argument value
                // Store it in the parameter's type for lookup
                param->type = getConstantType(ctx, args[argIndex]);
                argIndex++;
            }
        }
    }

    // ─── 3. Execute the body ─────────────────────────────────────────────
    ConstantValue result = ConstantValue::voidValue();
    if (func->body) {
        result = executeStmt(ctx, func->body);
    } else {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func,
                              "const function has no body");
        return ConstantValue::error();
    }

    // ─── 4. Check return type ────────────────────────────────────────────
    if (func->funcType && func->funcType->returnType) {
        if (result.isVoid()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func->body,
                                  "non-void const function does not return a value");
            return ConstantValue::error();
        }
    } else {
        if (!result.isVoid() && !result.isUnknown()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, func->body,
                                  "void const function returns a value");
            return ConstantValue::error();
        }
    }

    return result;
}

} // namespace sema