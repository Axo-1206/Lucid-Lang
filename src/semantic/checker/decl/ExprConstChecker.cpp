/**
 * @file ExprConstChecker.cpp
 * @brief Implementation of compile-time constant expression detection.
 * 
 * This file provides the isConstExpr function which determines if an
 * expression can be evaluated at compile time. Used for const variable
 * initializers, enum values, array sizes, and generic defaults.
 */

#include "DeclChecker.hpp"

bool isConstExpr(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return false;
    
    // If already marked constant, return cached result
    if (expr->isConst) return true;
    
    // Handle literal expressions
    if (auto* literal = expr->as<LiteralExprAST>()) {
        switch (literal->kind) {
            case LiteralKind::Int:
            case LiteralKind::Float:
            case LiteralKind::String:
            case LiteralKind::RawString:
            case LiteralKind::Char:
            case LiteralKind::True:
            case LiteralKind::False:
            case LiteralKind::Nil:
                expr->isConst = true;
                return true;
                
            case LiteralKind::Hex:
            case LiteralKind::Binary:
                // Hex and binary literals are integer literals
                expr->isConst = true;
                return true;
        }
        return false;
    }
    
    // Binary operations: const if both operands are const
    if (auto* binary = expr->as<BinaryExprAST>()) {
        bool leftConst = isConstExpr(binary->left, ctx);
        bool rightConst = isConstExpr(binary->right, ctx);
        
        if (leftConst && rightConst) {
            expr->isConst = true;
            return true;
        }
        return false;
    }
    
    // Unary operations: const if operand is const
    if (auto* unary = expr->as<UnaryExprAST>()) {
        if (isConstExpr(unary->operand, ctx)) {
            expr->isConst = true;
            return true;
        }
        return false;
    }
    
    // Identifier expressions: only const if they refer to a const variable
    // This is handled during type checking phase
    if (auto* ident = expr->as<IdentifierExprAST>()) {
        // Look up in scope - will be marked const during semantic analysis
        // For now, return false; const propagation happens in TypeChecker
        return false;
    }
    
    // All other expression types are not constant
    return false;
}