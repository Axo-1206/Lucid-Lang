/// @file const_eval/ConstEvaluator.cpp
/// @brief Main const evaluation logic - public API only.

#include "ConstEvaluator.hpp"
#include "ConstEvalHelpers.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"
#include "sema/Sema.hpp"

namespace sema {

// ─── Main Entry Points ───────────────────────────────────────────────────

ConstantValue ConstEvaluator::evaluateDecl(SemaContext& ctx, const VarDeclAST* decl) {
    if (!decl || !decl->init) {
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, decl,
                              "const variable '", ctx.pool.lookup(decl->name),
                              "' has no initializer");
        return ConstantValue::error();
    }

    if (m_recursionDepth >= MAX_RECURSION) {
        return ConstantValue::unknown();
    }

    if (m_evaluating.find(decl) != m_evaluating.end()) {
        ctx.diagnostics.error(DiagCode::Sem_CircularDependency, decl,
                              "circular dependency detected in const declaration '",
                              ctx.pool.lookup(decl->name), "'");
        return ConstantValue::error();
    }

    EvaluationGuard guard(m_evaluating, decl);
    m_recursionDepth++;

    ctx.pushScope();
    ctx.insertValue(decl);
    
    ConstantValue result = evaluate(ctx, decl->init, decl->type);
    
    ctx.popScope();
    m_recursionDepth--;

    return result;
}

ConstantValue ConstEvaluator::evaluate(SemaContext& ctx, const ExprAST* expr,
                                        const TypeAST* targetType) {
    if (!expr) return ConstantValue::error();

    if (m_recursionDepth >= MAX_RECURSION) {
        return ConstantValue::unknown();
    }

    // ─── Cache check ──────────────────────────────────────────────────────
    if (m_evaluatedExprs.find(expr) != m_evaluatedExprs.end()) {
        return expr->constValue;
    }

    ConstantValue result;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = evalLiteral(ctx, expr->as<LiteralExprAST>());
            break;
        case ASTKind::IdentifierExpr:
            result = evalIdentifier(ctx, expr->as<IdentifierExprAST>());
            break;
        case ASTKind::BinaryExpr:
            result = evalBinary(ctx, expr->as<BinaryExprAST>(), targetType);
            break;
        case ASTKind::UnaryExpr:
            result = evalUnary(ctx, expr->as<UnaryExprAST>(), targetType);
            break;
        case ASTKind::CallExpr:
            result = evalCall(ctx, expr->as<CallExprAST>());
            break;
        case ASTKind::StructLiteralExpr:
            result = evalStructLiteral(ctx, expr->as<StructLiteralExprAST>());
            break;
        case ASTKind::ArrayLiteralExpr:
            result = evalArrayLiteral(ctx, expr->as<ArrayLiteralExprAST>());
            break;
        case ASTKind::FieldAccessExpr:
            result = evalFieldAccess(ctx, expr->as<FieldAccessExprAST>());
            break;
        case ASTKind::NullCoalesceExpr:
            result = evalNullCoalesce(ctx, expr->as<NullCoalesceExprAST>());
            break;
        case ASTKind::IfExpr:
            result = evalIfExpr(ctx, expr->as<IfExprAST>());
            break;
        case ASTKind::RangeExpr:
            result = evalRangeExpr(ctx, expr->as<RangeExprAST>());
            break;
        default:
            return ConstantValue::unknown();
    }

    // ─── Store result on the AST node ──────────────────────────────────
    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(ctx, result);
        const_cast<ExprAST*>(expr)->valueState = result.isErr() ? ValueState::Err : ValueState::Definite;
        m_evaluatedExprs.insert(expr);
    }

    return result;
}

bool ConstEvaluator::isConstExpr(SemaContext& ctx, const ExprAST* expr,
                                  const TypeAST* targetType) {
    if (!expr) return false;
    if (expr->isConst) return true;
    
    ConstantValue val = evaluate(ctx, expr, targetType);
    return val.isEvaluated() && !val.isError();
}

ConstantValue ConstEvaluator::getConstValue(SemaContext& ctx, const ExprAST* expr,
                                             const TypeAST* targetType) {
    if (!expr) return ConstantValue::unknown();
    if (expr->isConst) return expr->constValue;
    return evaluate(ctx, expr, targetType);
}

std::optional<int64_t> ConstEvaluator::evaluateAsInt(SemaContext& ctx, const ExprAST* expr) {
    if (!expr) return std::nullopt;
    
    ConstantValue val = getConstValue(ctx, expr);
    if (val.isInt()) {
        return val.asInt();
    }
    return std::nullopt;
}

std::optional<bool> ConstEvaluator::evaluateAsBool(SemaContext& ctx, const ExprAST* expr) {
    if (!expr) return std::nullopt;
    
    ConstantValue val = getConstValue(ctx, expr);
    if (val.isBool()) {
        return val.asBool();
    }
    return std::nullopt;
}

// ─── evalRangeExpr ────────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalRangeExpr(SemaContext& ctx, const RangeExprAST* expr) {
    if (!expr) return ConstantValue::error();

    // ─── Evaluate both bounds ───────────────────────────────────────────
    ConstantValue loVal = evaluate(ctx, expr->lo);
    if (loVal.isError()) return loVal;
    if (loVal.isUnknown()) return ConstantValue::unknown();

    ConstantValue hiVal = evaluate(ctx, expr->hi);
    if (hiVal.isError()) return hiVal;
    if (hiVal.isUnknown()) return ConstantValue::unknown();

    // ─── Both bounds must be integers ────────────────────────────────────
    if (!loVal.isInt() || !hiVal.isInt()) {
        return ConstantValue::unknown();
    }

    int64_t lo = loVal.asInt();
    int64_t hi = hiVal.asInt();
    bool isInclusive = !expr->isExclusive;
    
    // ─── Validate range order ────────────────────────────────────────────
    if (isInclusive && lo > hi) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidRange, expr,
                              "inclusive range start (", lo, 
                              ") must be less than or equal to end (", hi, ")");
        return ConstantValue::error();
    }
    if (!isInclusive && lo >= hi) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidRange, expr,
                              "exclusive range start (", lo, 
                              ") must be less than end (", hi, ")");
        return ConstantValue::error();
    }

    // ─── Return the lower bound (the range itself is a "value") ──────────
    // For a range, we return the lower bound as the "value"
    // The caller can access loVal and hiVal separately if needed
    return loVal;
}

// ─── evalCall ─────────────────────────────────────────────────────────────

ConstantValue ConstEvaluator::evalCall(SemaContext& ctx, const CallExprAST* expr) {
    const FuncDeclAST* func = resolveCalleeOrError(expr->callee, ctx);
    if (!func) {
        return ConstantValue::error();
    }

    if (func->keyword != DeclKeyword::Const) {
        return ConstantValue::unknown();
    }

    if (!func->genericParams.empty() && expr->genericArgs.empty()) {
        return ConstantValue::unknown();
    }

    std::vector<ConstantValue> args;
    for (const ExprAST* arg : expr->args) {
        ConstantValue val = evaluate(ctx, arg);
        if (val.isError()) return val;
        if (val.isUnknown()) return ConstantValue::unknown();
        args.push_back(val);
    }

    return executeFunction(ctx, func, args);
}

// ─── executeFunction ──────────────────────────────────────────────────────

ConstantValue ConstEvaluator::executeFunction(SemaContext& ctx, const FuncDeclAST* func,
                                               const std::vector<ConstantValue>& args) {
    if (!func) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                              "null function");
        return ConstantValue::error();
    }

    ConstFunctionContext context(ctx, func);

    size_t argIndex = 0;
    for (FuncTypeAST* group = func->funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            if (argIndex < args.size()) {
                const_cast<ParamAST*>(param)->type = getConstantType(ctx, args[argIndex]);
                argIndex++;
            }
        }
    }

    ConstantValue result = ConstantValue::voidValue();
    if (func->body) {
        result = executeStmt(ctx, func->body);
    } else {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, func,
                              "const function has no body");
        return ConstantValue::error();
    }

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

// ─── Report Cycle ────────────────────────────────────────────────────────

void ConstEvaluator::reportCycle(SemaContext& ctx, const std::vector<const DeclAST*>& cycle) {
    if (cycle.empty()) return;
    
    std::string msg = "circular dependency in const declarations: ";
    for (size_t i = 0; i < cycle.size(); ++i) {
        if (i > 0) msg += " → ";
        msg += ctx.pool.lookup(cycle[i]->name);
    }
    ctx.diagnostics.error(DiagCode::Sem_CircularDependency, cycle[0], msg);
}

ConstantValue ConstEvaluator::getConstValue(const VarDeclAST* decl) {
    if (!decl || decl->keyword != DeclKeyword::Const || !decl->init) {
        return ConstantValue::unknown();
    }
    if (decl->init->isConst) {
        return decl->init->constValue;
    }
    return ConstantValue::unknown();
}

void ConstEvaluator::buildDependencyGraph(SemaContext& ctx) {
    m_constDecls.clear();
    m_deps.clear();

    for (ModuleAST* module : ctx.modules) {
        for (const DeclPtr decl : module->decls) {
            if (decl && decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const) {
                    m_constDecls.push_back(var);
                }
            }
            if (decl && decl->isa<FuncDeclAST>()) {
                const FuncDeclAST* func = decl->as<FuncDeclAST>();
                if (func->keyword == DeclKeyword::Const) {
                    m_constDecls.push_back(func);
                }
            }
        }
    }

    for (const DeclAST* decl : m_constDecls) {
        std::vector<const DeclAST*> deps;
        if (decl->isa<VarDeclAST>()) {
            const VarDeclAST* var = decl->as<VarDeclAST>();
            if (var->init) {
                collectDeps(ctx, var->init, deps);
            }
        } else if (decl->isa<FuncDeclAST>()) {
            const FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->body) {
                collectDepsFromStmt(ctx, func->body, deps);
            }
        }
        m_deps[decl] = deps;
    }

    topologicalSort(ctx, m_deps);
}

} // namespace sema