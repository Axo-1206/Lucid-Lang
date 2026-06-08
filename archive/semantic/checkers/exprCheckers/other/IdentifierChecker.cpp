/**
 * @file IdentifierChecker.cpp
 * @brief Semantic checking for identifier expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkIdentifierExpr(IdentifierExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIdentifierExpr: name=" << ctx.pool.lookup(node.name));
    
    // Look up symbol in current scope
    Symbol* sym = ctx.symbols->lookup(node.name);
    if (!sym) {
        ctx.error(node.loc, DiagCode::E2001,
                  "undefined identifier '", ctx.pool.lookup(node.name), "'");
        return nullptr;
    }
    
    // Get symbol type
    TypeAST* type = sym->type;
    
    // If type not resolved yet, try to resolve from declaration
    if (!type && sym->decl) {
        if (auto* varDecl = sym->decl->as<VarDeclAST>()) {
            if (varDecl->type && ctx.dispatcher) {
                type = ctx.dispatcher->resolveType(varDecl->type.get());
                sym->type = type;
            }
        } else if (auto* funcDecl = sym->decl->as<FuncDeclAST>()) {
            if (funcDecl->funcType && ctx.dispatcher) {
                type = ctx.dispatcher->resolveType(funcDecl->funcType.get());
                sym->type = type;
            }
        } else if (auto* param = sym->decl->as<ParamAST>()) {
            if (param->type && ctx.dispatcher) {
                type = ctx.dispatcher->resolveType(param->type.get());
                sym->type = type;
            }
        }
    }
    
    if (!type) {
        ctx.error(node.loc, DiagCode::E2001,
                  "identifier '", ctx.pool.lookup(node.name), "' has no known type");
        return nullptr;
    }
    
    // Set const flag for const declarations
    if (sym->declKw == DeclKeyword::Const) {
        node.isConst = true;
    }
    
    // For enum types, mark as const (enum values are compile-time constants)
    if (sym->kind == SymbolKind::Enum) {
        node.isConst = true;
    }
    
    node.resolvedType = type;
    return type;
}