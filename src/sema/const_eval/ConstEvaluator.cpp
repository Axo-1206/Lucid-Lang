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
        ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, decl,
                              "const evaluation recursion limit exceeded (",
                              MAX_RECURSION, ")");
        return ConstantValue::error();
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
        ctx.diagnostics.error(DiagCode::Sem_ConstEvalLimit, expr,
                              "const evaluation recursion limit exceeded (",
                              MAX_RECURSION, ")");
        return ConstantValue::error();
    }

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
        default:
            return ConstantValue::unknown();
    }

    if (result.isEvaluated() && !result.isError()) {
        const_cast<ExprAST*>(expr)->isConst = true;
        const_cast<ExprAST*>(expr)->constValue = result;
        const_cast<ExprAST*>(expr)->resolvedType = getConstantType(ctx, result);
        const_cast<ExprAST*>(expr)->valueState = result.isErr() ? ValueState::Err : ValueState::Definite;
        m_evaluatedExprs.insert(expr);
    }

    return result;
}

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
    // Collect all const declarations
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

    // Build dependency graph
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

    // Check for cycles
    topologicalSort(ctx, m_deps);
}

} // namespace sema