/**
 * @file BehaviorAccessChecker.cpp
 * @brief Semantic checking for method access expressions (Type:method).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkBehaviorAccessExpr: type=" << ctx.pool.lookup(node.typeName)
                             << ", method=" << ctx.pool.lookup(node.method));
    
    // Look up the type
    Symbol* sym = ctx.symbols->lookup(node.typeName);
    if (!sym || (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Trait)) {
        ctx.error(node.loc, DiagCode::E2001,
                  "unknown type '", ctx.pool.lookup(node.typeName),
                  "' for method access");
        return nullptr;
    }
    
    // Build mangled method name: Type::method
    std::string mangled = NameMangler::mangleMethod(
        ctx.pool.lookup(node.typeName),
        ctx.pool.lookup(node.method));
    
    Symbol* methodSym = ctx.symbols->lookup(ctx.pool.intern(mangled));
    if (!methodSym || (methodSym->kind != SymbolKind::Method && methodSym->kind != SymbolKind::Func)) {
        ctx.error(node.loc, DiagCode::E2001,
                  "type '", ctx.pool.lookup(node.typeName),
                  "' has no method '", ctx.pool.lookup(node.method), "'");
        return nullptr;
    }
    
    // Resolve method type if not already resolved
    TypeAST* methodType = methodSym->type;
    if (!methodType) {
        if (auto* methodDecl = methodSym->decl->as<MethodDeclAST>()) {
            if (methodDecl->funcType && ctx.dispatcher) {
                methodType = ctx.dispatcher->resolveType(methodDecl->funcType.get());
                methodSym->type = methodType;
            }
        } else if (auto* traitMethod = methodSym->decl->as<TraitMethodAST>()) {
            if (traitMethod->funcType && ctx.dispatcher) {
                methodType = ctx.dispatcher->resolveType(traitMethod->funcType.get());
                methodSym->type = methodType;
            }
        }
    }
    
    if (!methodType) {
        ctx.error(node.loc, DiagCode::E2002, "method type could not be resolved");
        return nullptr;
    }
    
    node.resolvedType = methodType;
    node.isBehaviorMember = true;
    node.isConst = false; // Method references are runtime values
    
    // Store concrete type arguments for codegen
    if (!node.genericArgs.empty()) {
        std::vector<InternedString> concreteArgs;
        for (const auto& arg : node.genericArgs) {
            if (arg && arg->isa<NamedTypeAST>()) {
                concreteArgs.push_back(arg->as<NamedTypeAST>()->name);
            }
        }
        auto builder = ctx.arena.makeBuilder<InternedString>();
        for (const auto& ca : concreteArgs) builder.push_back(ca);
        node.concreteTypeArgs = builder.build();
    }
    
    return methodType;
}