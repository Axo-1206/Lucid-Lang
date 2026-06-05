/**
 * @file MatchChecker.cpp
 * @brief Semantic checking for match expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include <unordered_set>

// Forward declaration for pattern checking (future implementation)
static bool checkPattern(PatternAST* pattern, TypeAST* subjectType, SemanticContext& ctx);

TypeAST* checkMatchExpr(MatchExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkMatchExpr: arms=" << node.arms.size());
    
    // Check subject expression
    TypeAST* subjectType = checkExpr(node.subject.get(), ctx);
    if (!subjectType) return nullptr;
    
    // Validate that default arm is present
    if (!node.defaultBody) {
        ctx.error(node.loc, DiagCode::E2041, "match expression must have a 'default' arm");
        return nullptr;
    }
    
    // Track seen patterns for duplicate detection (simplified)
    std::unordered_set<std::string> seenPatterns;
    TypeAST* unifiedType = nullptr;
    
    // Check each non-default arm
    for (auto& arm : node.arms) {
        if (!arm) continue;
        
        // Check patterns
        for (auto& pattern : arm->patterns) {
            if (!pattern) continue;
            
            // TODO: Implement proper pattern checking
            // For now, basic validation
            if (pattern->isa<BindPatternAST>()) {
                auto* bind = pattern->as<BindPatternAST>();
                // Bind pattern introduces a new variable
                Symbol bindSym;
                bindSym.name = bind->name;
                bindSym.kind = SymbolKind::Var;
                bindSym.declKw = DeclKeyword::Let;
                bindSym.visibility = Visibility::Private;
                bindSym.type = subjectType;
                bindSym.decl = pattern.get();
                bindSym.loc = pattern->loc;
                
                // Push to current scope (handled by caller, not here)
                // We'll just log for now
                LUC_LOG_SEMANTIC_EXTREME("bind pattern: " << ctx.pool.lookup(bind->name));
            }
            
            // Check pattern compatibility with subject type
            // This would validate that e.g., range patterns are on integers,
            // type patterns are on any, etc.
        }
        
        // Check guard expression if present
        if (arm->guard) {
            TypeAST* guardType = checkExpr(arm->guard.get(), ctx);
            if (guardType && !TypeChecker::isBooleanCompatible(guardType, ctx)) {
                ctx.error(arm->guard->loc, DiagCode::E2002, "match guard must be boolean");
                return nullptr;
            }
        }
        
        // Check arm expressions
        for (auto& expr : arm->exprs) {
            TypeAST* exprType = checkExpr(expr.get(), ctx);
            if (!exprType) return nullptr;
            
            if (!unifiedType) {
                unifiedType = exprType;
            } else {
                unifiedType = TypeChecker::unify(unifiedType, exprType, ctx);
                if (!unifiedType) {
                    ctx.error(expr->loc, DiagCode::E2002,
                              "match arms have incompatible types: expected '",
                              LucDebug::formatType(unifiedType, ctx.pool), "', got '",
                              LucDebug::formatType(exprType, ctx.pool), "'");
                    return nullptr;
                }
            }
        }
    }
    
    // Check default arm expressions
    for (auto& expr : node.defaultBody->exprs) {
        TypeAST* exprType = checkExpr(expr.get(), ctx);
        if (!exprType) return nullptr;
        
        if (!unifiedType) {
            unifiedType = exprType;
        } else {
            unifiedType = TypeChecker::unify(unifiedType, exprType, ctx);
            if (!unifiedType) {
                ctx.error(expr->loc, DiagCode::E2002,
                          "default arm type does not match other arms: expected '",
                          LucDebug::formatType(unifiedType, ctx.pool), "', got '",
                          LucDebug::formatType(exprType, ctx.pool), "'");
                return nullptr;
            }
        }
    }
    
    // Check for unreachable patterns (simplified)
    // A more sophisticated implementation would track exhaustiveness
    bool hasWildcard = false;
    for (auto& arm : node.arms) {
        for (auto& pattern : arm->patterns) {
            if (pattern->isa<WildcardPatternAST>()) {
                hasWildcard = true;
                if (&arm != &node.arms.back()) {
                    ctx.warning(pattern->loc, DiagCode::W6009,
                                "wildcard pattern before final arm may make later arms unreachable");
                }
                break;
            }
        }
    }
    
    node.resolvedType = unifiedType;
    return unifiedType;
}