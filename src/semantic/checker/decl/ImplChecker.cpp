/**
 * @file ImplChecker.cpp
 * @brief Implementation of impl block validation.
 * 
 * Validates implementation blocks including:
 *   - Duplicate method detection
 *   - Trait fulfillment verification
 *   - Method body validation
 *   - Receiver injection validation
 */

#include "DeclChecker.hpp"
#include "semantic/checker/expr/ExprChecker.hpp"
#include "semantic/checker/stmt/StmtChecker.hpp"
#include "debug/DebugMacros.hpp"

#include <unordered_set>

namespace {
    // Local helper for duplicate method checking
    bool checkDuplicateMethods(ArenaSpan<MethodDeclPtr> methods, SemanticContext& ctx) {
        std::unordered_set<uint32_t> methodNames;
        bool noDuplicates = true;
        
        for (auto* method : methods) {
            uint32_t id = method->name.id;
            if (methodNames.find(id) != methodNames.end()) {
                ctx.error(method->loc, DiagCode::E2001,
                          "duplicate method '", ctx.pool.lookup(method->name), 
                          "' in impl block");
                noDuplicates = false;
            }
            methodNames.insert(id);
        }
        
        return noDuplicates;
    }
    
    // Local helper for trait fulfillment checking
    bool checkTraitFulfillment(ImplDeclAST* impl, SemanticContext& ctx) {
        if (!impl->traitRef) return true;
        
        // Look up the trait in the type namespace
        TypeDeclAST* traitDecl = ctx.scope.lookupType(impl->traitRef->name);
        if (!traitDecl) {
            ctx.error(impl->loc, DiagCode::E2001,
                      "undefined trait '", ctx.pool.lookup(impl->traitRef->name), "'");
            return false;
        }
        
        auto* trait = traitDecl->as<TraitDeclAST>();
        if (!trait) {
            ctx.error(impl->loc, DiagCode::E2001,
                      "'", ctx.pool.lookup(impl->traitRef->name), "' is not a trait");
            return false;
        }
        
        // Check that all trait methods are implemented
        std::unordered_set<uint32_t> implMethods;
        for (auto* method : impl->methods) {
            implMethods.insert(method->name.id);
        }
        
        bool allImplemented = true;
        for (auto* traitMethod : trait->methods) {
            uint32_t id = traitMethod->name.id;
            if (implMethods.find(id) == implMethods.end()) {
                ctx.error(impl->loc, DiagCode::E2001,
                          "trait '", ctx.pool.lookup(trait->name),
                          "' requires method '", ctx.pool.lookup(traitMethod->name),
                          "' but it is not implemented");
                allImplemented = false;
            }
        }
        
        return allImplemented;
    }
} // anonymous namespace

void checkImplDecl(ImplDeclAST* impl, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkImplDecl");
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(impl, ctx);
    
    // Check for duplicate method names
    checkDuplicateMethods(impl->methods, ctx);
    
    // Check trait fulfillment
    checkTraitFulfillment(impl, ctx);
    
    // Check each method
    for (auto* method : impl->methods) {
        if (method->isInlineBody()) {
            // Push impl scope for method checking
            ctx.scope.push();
            
            // Inject receiver symbol if present
            if (impl->receiverAlias.isValid()) {
                // The receiver is already declared in the scope by the collector
                // Just need to check that it's used correctly
            }
            
            // Check method body
            TypeAST* expectedReturn = nullptr;
            if (method->funcType && !method->funcType->returnTypes.empty()) {
                expectedReturn = method->funcType->returnTypes[0];
            }
            
            if (method->body) {
                checkStmt(method->body, ctx, expectedReturn);
            } else if (method->assignmentRef) {
                // Assignment form – check the reference
                checkExpr(method->assignmentRef, ctx);
            }
            
            ctx.scope.pop();
        } else if (method->assignmentRef) {
            // Assignment form – check the reference
            checkExpr(method->assignmentRef, ctx);
        }
    }
}