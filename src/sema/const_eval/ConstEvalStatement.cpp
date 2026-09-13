/// @file const_eval/ConstEvalStatement.cpp
/// @brief Statement execution for const functions.

#include "ConstEvalHelpers.hpp"
#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaType.hpp"
#include "sema/Sema.hpp"
#include "sema/support/Truthiness.hpp"

namespace sema {

// ─── Statement Execution ──────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeStmt(SemaContext& ctx, StmtAST* stmt) {
    if (!stmt) return ConstantValue::voidValue();
        if (stmt->hasSyntaxError) return ConstantValue::error();

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

    bool condition = constantTruthiness(cond, ctx);

    // ─── 3. Get narrowing info detected during condition evaluation ────
    NarrowingInfo info = ctx.stack.getPendingNarrowing();
    ctx.stack.clearPendingNarrowing();

    // ─── 4. Execute the appropriate branch ──────────────────────────────
    if (condition) {
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

        if (!constantTruthiness(cond, ctx)) break;

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
    // ─── 0. Guard: nothing to execute ─────────────────────────────────────
    //
    // A FuncDeclAST is a declaration, not a body. The body lives on the
    // AnonFuncExprAST at `init` (when the declaration has one), or nowhere
    // at all (foreign function, reference alias). Both cases mean there is
    // nothing for the const evaluator to execute, so we return `unknown`
    // rather than `error`: this is a "can't be const-evaluated" outcome,
    // not a user mistake.
    if (!func || !func->init) {
        return ConstantValue::unknown();
    }
    if (!func->init->isa<AnonFuncExprAST>()) {
        return ConstantValue::unknown();
    }

    AnonFuncExprAST* body = func->init->as<AnonFuncExprAST>();
    if (!body->funcType || !body->body) {
        return ConstantValue::unknown();
    }

    // ─── 1. Push the function's context and parameter scope ──────────────
    //
    // ConstFunctionContext pushes:
    //   - a FuncBody frame on the context stack (for return-type checks,
    //     enclosing-function tracking, and any narrowing that needs to
    //     know we're inside a function)
    //   - a symbol scope (so parameters and locals declared in the body
    //     have somewhere to live)
    //
    // Both are popped automatically when the guard goes out of scope,
    // including on any early-return path below.
    ConstFunctionContext guard(ctx, func);

    // ─── 2. Bind parameters ──────────────────────────────────────────────
    //
    // Read parameters from `body->funcType`, NOT `func->funcType`. The
    // FuncDeclAST doc-comment (DeclAST.hpp) is explicit about this: the
    // declared signature's ParamAST nodes are type-only and are never
    // registered as bindings; the runtime signature is the one on the
    // AnonFuncExprAST, and its ParamAST nodes are the ones the body's
    // identifiers resolve to.
    //
    // A single call of a curried function supplies one group's worth of
    // arguments, and `body->funcType->params` is exactly that group.
    // We do not walk the curried return chain here — that would flatten
    // several groups onto one call's args and misalign them.
    const auto& params = body->funcType->params;
    if (params.size() != args.size()) {
        // Arity is checked by Sema before this point is reachable; a
        // mismatch here means the compiler built a call node with the
        // wrong number of arguments, which is a compiler bug, not a
        // user error. Returning `unknown` keeps the evaluator's
        // contract ("either a value or a clean non-value") without
        // spurious diagnostics.
        return ConstantValue::unknown();
    }

    std::vector<ParamAST*> bound;
    bound.reserve(params.size());

    for (size_t i = 0; i < params.size(); ++i) {
        ParamAST* param = params[i];
        if (!param) continue;

        // `_` discard — no name to bind, nothing to do.
        if (param->name.isEmpty()) continue;

        // Register the parameter in the current scope so that name
        // lookup from the body finds it. insertValue diagnoses a
        // redeclaration, but parameter names were already uniqueness-
        // checked by Sema against the function's own scope, so this
        // should never fire.
        ctx.insertValue(param);

        // Bind the value in the side table. evalIdentifier's ParamAST
        // branch consults this table; see below for why a side table
        // rather than a field on ParamAST.
        m_paramBindings[param] = args[i];

        bound.push_back(param);
    }

    // ─── 3. Execute the body ─────────────────────────────────────────────
    //
    // executeStmt returns:
    //   - a Definite ConstantValue: the function produced a value via
    //     a `return expr` (or the body's last statement was an
    //     expression yielding a value).
    //   - ConstantValue::voidValue(): the function returned bare
    //     `return`, or fell off the end without returning anything.
    //   - ConstantValue::unknown(): the body contains something that
    //     can't be const-evaluated (an I/O call, a mutable binding,
    //     an unbounded loop).
    //   - ConstantValue::error(): the body contained a definite error
    //     (a diagnostic has already been emitted).
    ConstantValue result;
    if (body->body->hasSyntaxError) {
        result = ConstantValue::error();
    } else {
        result = executeStmt(ctx, body->body);
    }

    // ─── 4. Check the return type ────────────────────────────────────────
    //
    // The declared return type comes from `func->funcType` (the declared
    // signature), not from `body->funcType` — the declared signature is
    // what the caller sees, and it's what determines "should this have
    // returned a value?". Under substitution both are the same shape, so
    // reading either is fine; reading the declared one keeps the check
    // in terms of what the source says.
    if (func->funcType && func->funcType->returnType) {
        if (result.isVoid()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingReturn, body->body,
                                  "non-void const function does not return a value");
            result = ConstantValue::error();
        }
    } else {
        // Declared void. A non-void, non-unknown result means the body
        // returned something the signature didn't promise.
        if (!result.isVoid() && !result.isUnknown() && !result.isError()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, body->body,
                                  "void const function returns a value");
            result = ConstantValue::error();
        }
    }

    // ─── 5. Unbind parameters ────────────────────────────────────────────
    //
    // Cleanup happens explicitly rather than via an RAII guard because
    // the set of parameters bound this call is only known here — the
    // ConstFunctionContext guard covers the scope and function frame,
    // but the specific ParamAST*s bound are this function's business.
    //
    // Erasing in reverse isn't necessary for a hash map keyed by pointer
    // (there's no ordering dependency), but it mirrors the LIFO cleanup
    // convention used elsewhere in the evaluator and costs nothing.
    for (auto it = bound.rbegin(); it != bound.rend(); ++it) {
        m_paramBindings.erase(*it);
    }

    return result;
}

} // namespace sema