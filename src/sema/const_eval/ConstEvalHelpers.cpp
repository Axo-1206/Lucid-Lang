/// @file const_eval/ConstEvalHelpers.cpp
/// @brief Implementation of const evaluation helpers.

#include "ConstEvalHelpers.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"
#include "sema/Sema.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <queue>
#include <algorithm>

namespace sema {

// ─── Type Helpers ──────────────────────────────────────────────────────

TypeAST* getConstantType(SemaContext& ctx, const ConstantValue& val) {
    if (val.type) return val.type;

    switch (val.kind) {
        case ConstantValue::Kind::Bool:   return ctx.getBoolType();
        case ConstantValue::Kind::Int:    return ctx.getIntType();
        case ConstantValue::Kind::Float:  return ctx.getFloatType();
        case ConstantValue::Kind::String: return ctx.getStringType();
        case ConstantValue::Kind::Char:   return ctx.getCharType();
        default:                          return ctx.getUnknownType();
    }
}

double toDouble(const ConstantValue& v) {
    if (v.isInt()) return static_cast<double>(v.asInt());
    if (v.isFloat()) return v.asFloat();
    return 0.0;
}

bool bothNumeric(const ConstantValue& a, const ConstantValue& b) {
    return (a.isInt() || a.isFloat()) && (b.isInt() || b.isFloat());
}

// ─── Error Helpers ──────────────────────────────────────────────────────

ConstantValue handleArithmeticError(SemaContext& ctx, 
                                     const char* op, 
                                     const std::string& reason,
                                     const BaseAST* node,
                                     const TypeAST* targetType) {
    if (targetType && isFallibleType(targetType)) {
        return ConstantValue::err();
    }
    ctx.diagnostics.error(DiagCode::Sem_DivisionByZero, node,
                          reason, " in const expression (", op, " operation)");
    return ConstantValue::error();
}

// ─── Dependency Helpers ─────────────────────────────────────────────────

void collectDeps(SemaContext& ctx, const ExprAST* expr,
                  std::vector<const DeclAST*>& deps) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            const ValueDeclAST* decl = ctx.lookupValue(id->name);
            if (decl && decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const) {
                    deps.push_back(var);
                }
            }
            if (decl && decl->isa<FuncDeclAST>()) {
                const FuncDeclAST* func = decl->as<FuncDeclAST>();
                if (func->keyword == DeclKeyword::Const) {
                    deps.push_back(func);
                }
            }
            break;
        }
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            collectDeps(ctx, bin->left, deps);
            collectDeps(ctx, bin->right, deps);
            break;
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            collectDeps(ctx, unary->operand, deps);
            break;
        }
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            collectDeps(ctx, call->callee, deps);
            for (const ExprAST* arg : call->args) {
                collectDeps(ctx, arg, deps);
            }
            break;
        }
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            collectDeps(ctx, field->object, deps);
            break;
        }
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* sl = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : sl->inits) {
                collectDeps(ctx, init->value, deps);
            }
            break;
        }
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* al = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : al->elements) {
                collectDeps(ctx, elem, deps);
            }
            break;
        }
        default:
            break;
    }
}

void collectDepsFromStmt(SemaContext& ctx, const StmtAST* stmt,
                          std::vector<const DeclAST*>& deps) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            const BlockStmtAST* block = stmt->as<BlockStmtAST>();
            for (const StmtAST* s : block->stmts) {
                collectDepsFromStmt(ctx, s, deps);
            }
            break;
        }
        case ASTKind::ExprStmt: {
            const ExprStmtAST* exprStmt = stmt->as<ExprStmtAST>();
            collectDeps(ctx, exprStmt->expr, deps);
            break;
        }
        case ASTKind::ReturnStmt: {
            const ReturnStmtAST* ret = stmt->as<ReturnStmtAST>();
            if (ret->value) {
                collectDeps(ctx, ret->value, deps);
            }
            break;
        }
        case ASTKind::IfStmt: {
            const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            collectDeps(ctx, ifStmt->condition, deps);
            collectDepsFromStmt(ctx, ifStmt->thenBranch, deps);
            if (ifStmt->elseBranch) {
                collectDepsFromStmt(ctx, ifStmt->elseBranch, deps);
            }
            break;
        }
        case ASTKind::WhileStmt: {
            const WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            collectDeps(ctx, whileStmt->condition, deps);
            collectDepsFromStmt(ctx, whileStmt->body, deps);
            break;
        }
        case ASTKind::DeclStmt: {
            const DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            if (declStmt->decl->isa<VarDeclAST>()) {
                const VarDeclAST* var = declStmt->decl->as<VarDeclAST>();
                if (var->keyword == DeclKeyword::Const && var->init) {
                    collectDeps(ctx, var->init, deps);
                }
            }
            break;
        }
        default:
            break;
    }
}

std::vector<const DeclAST*> topologicalSort(SemaContext& ctx,
                                             const std::unordered_map<const DeclAST*, std::vector<const DeclAST*>>& deps) {
    std::vector<const DeclAST*> result;
    std::unordered_map<const DeclAST*, size_t> inDegree;
    std::unordered_map<const DeclAST*, std::vector<const DeclAST*>> graph;

    for (const auto& [decl, depsList] : deps) {
        inDegree[decl] = 0;
        graph[decl] = {};
    }

    for (const auto& [decl, depsList] : deps) {
        for (const DeclAST* dep : depsList) {
            if (graph.find(dep) == graph.end()) continue;
            graph[decl].push_back(dep);
            inDegree[dep]++;
        }
    }

    std::queue<const DeclAST*> queue;
    for (const auto& [decl, degree] : inDegree) {
        if (degree == 0) {
            queue.push(decl);
        }
    }

    while (!queue.empty()) {
        const DeclAST* decl = queue.front();
        queue.pop();
        result.push_back(decl);

        for (const DeclAST* dep : graph[decl]) {
            inDegree[dep]--;
            if (inDegree[dep] == 0) {
                queue.push(dep);
            }
        }
    }

    if (result.size() != deps.size()) {
        std::vector<const DeclAST*> cycle;
        for (const auto& [decl, degree] : inDegree) {
            if (degree > 0) {
                cycle.push_back(decl);
            }
        }
        // Report cycle through the context
        if (!cycle.empty()) {
            std::string msg = "circular dependency in const declarations: ";
            for (size_t i = 0; i < cycle.size(); ++i) {
                if (i > 0) msg += " → ";
                msg += ctx.pool.lookup(cycle[i]->name);
            }
            ctx.diagnostics.error(DiagCode::Sem_CircularDependency, cycle[0], msg);
        }
    }

    return result;
}

} // namespace sema