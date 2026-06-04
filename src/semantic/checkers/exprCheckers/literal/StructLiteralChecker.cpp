/**
 * @file StructLiteralChecker.cpp
 * @brief Semantic checking for struct literal expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/resolveType/TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include <unordered_set>

TypeAST* checkStructLiteralExpr(StructLiteralExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkStructLiteralExpr: type=" << ctx.pool.lookup(node.typeName));
    
    // Look up the struct type
    Symbol* sym = ctx.symbols->lookup(node.typeName);
    if (!sym || sym->kind != SymbolKind::Struct) {
        ctx.error(node.loc, DiagCode::E2001,
                  "unknown struct type '", ctx.pool.lookup(node.typeName), "'");
        return nullptr;
    }
    
    auto* structDecl = sym->decl->as<StructDeclAST>();
    
    // Handle generic arguments if present
    if (!node.genericArgs.empty()) {
        auto* instantiated = ctx.arena.make<NamedTypeAST>(node.typeName).release();
        instantiated->genericArgs = node.genericArgs;
        for (auto& arg : instantiated->genericArgs) {
            if (ctx.resolver) {
                ctx.resolver->resolveType(arg.get());
            }
        }
        node.instantiatedType = ASTPtr<NamedTypeAST>(instantiated);
    }
    
    // Determine the struct's type
    TypeAST* structType = node.instantiatedType ? node.instantiatedType.get()
                         : structDecl->selfType.get();
    if (!structType) {
        ctx.error(node.loc, DiagCode::E2001, "struct type not resolved");
        return nullptr;
    }
    
    // Track which fields have been initialised
    std::unordered_set<InternedString> fieldSeen;
    
    // Check each field initialiser
    for (auto& init : node.inits) {
        if (!init) continue;
        
        bool fieldFound = false;
        for (auto& field : structDecl->fields) {
            if (field->name == init->name) {
                fieldFound = true;
                
                // Check initialiser type
                TypeAST* initType = checkExpr(init->value.get(), ctx);
                if (!initType) return nullptr;
                
                if (!TypeChecker::isAssignable(initType, field->type.get(), ctx)) {
                    ctx.error(init->value->loc, DiagCode::E2002,
                              "initialiser type mismatch for field '", ctx.pool.lookup(init->name), "'");
                    return nullptr;
                }
                
                fieldSeen.insert(init->name);
                break;
            }
        }
        
        if (!fieldFound) {
            ctx.error(init->loc, DiagCode::E2001,
                      "struct '", ctx.pool.lookup(node.typeName),
                      "' has no field '", ctx.pool.lookup(init->name), "'");
            return nullptr;
        }
    }
    
    // Check for missing required fields (no default value)
    for (auto& field : structDecl->fields) {
        if (!fieldSeen.count(field->name) && !field->defaultVal) {
            ctx.error(node.loc, DiagCode::E2001,
                      "missing initialiser for field '", ctx.pool.lookup(field->name), "'");
            return nullptr;
        }
    }
    
    node.resolvedType = structType;
    return structType;
}