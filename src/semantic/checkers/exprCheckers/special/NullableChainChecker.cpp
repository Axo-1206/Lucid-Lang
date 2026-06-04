/**
 * @file NullableChainChecker.cpp
 * @brief Semantic checking for nullable chain expressions (?.).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkNullableChainExpr(NullableChainExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkNullableChainExpr: steps=" << node.steps.size());
    
    // Check root object
    TypeAST* rootType = checkExpr(node.object.get(), ctx);
    if (!rootType) return nullptr;
    
    // Root must be nullable
    if (!TypeChecker::isNullable(rootType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002,
                  "nullable chain (?.) requires nullable left-hand side");
        return nullptr;
    }
    
    TypeAST* current = rootType;
    
    // Traverse each step
    for (size_t i = 0; i < node.steps.size(); ++i) {
        InternedString fieldName = node.steps[i];
        
        // Current must be nullable
        if (!TypeChecker::isNullable(current, ctx)) {
            ctx.error(node.loc, DiagCode::E2002,
                      "internal error: nullable chain expected nullable type at step ", i + 1);
            return nullptr;
        }
        
        // Get inner type (unwrap nullable)
        TypeAST* inner = current->isa<NullableTypeAST>()
                         ? current->as<NullableTypeAST>()->inner.get()
                         : current;
        
        // Inner must be a named type (struct)
        if (!inner->isa<NamedTypeAST>()) {
            ctx.error(node.loc, DiagCode::E2002,
                      "nullable chain only supports struct fields (got '",
                      LucDebug::formatType(inner, ctx.pool), "')");
            return nullptr;
        }
        
        // Look up struct
        Symbol* structSym = ctx.symbols->lookup(inner->as<NamedTypeAST>()->name);
        if (!structSym || structSym->kind != SymbolKind::Struct) {
            ctx.error(node.loc, DiagCode::E2001,
                      "type '", ctx.pool.lookup(inner->as<NamedTypeAST>()->name),
                      "' not found for nullable chain");
            return nullptr;
        }
        
        // Find field
        auto* structDecl = structSym->decl->as<StructDeclAST>();
        TypeAST* fieldType = nullptr;
        
        for (auto& field : structDecl->fields) {
            if (field->name == fieldName) {
                fieldType = field->type.get();
                break;
            }
        }
        
        if (!fieldType) {
            ctx.error(node.loc, DiagCode::E2001,
                      "struct '", ctx.pool.lookup(inner->as<NamedTypeAST>()->name),
                      "' has no field '", ctx.pool.lookup(fieldName), "'");
            return nullptr;
        }
        
        // Wrap field type as nullable for next step (unless it's the last step)
        if (i < node.steps.size() - 1) {
            // Field becomes nullable because the chain may short-circuit
            current = ctx.arena.make<NullableTypeAST>(TypePtr(fieldType)).release();
        } else {
            // Last step: field type remains as-is (nullability will be resolved by ??)
            current = fieldType;
        }
    }
    
    // The chain result is nullable (the whole chain may be nil)
    // The grammar requires this to be terminated by ??, which will unwrap it
    TypeAST* result = ctx.arena.make<NullableTypeAST>(TypePtr(current)).release();
    node.resolvedType = result;
    node.isConst = false; // Chain result depends on runtime null checks
    return result;
}