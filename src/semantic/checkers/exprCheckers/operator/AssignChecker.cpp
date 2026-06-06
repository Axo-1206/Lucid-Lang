/**
 * @file AssignChecker.cpp
 * @brief Semantic checking for assignment expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkAssignExpr(AssignExprAST& node, SemanticContext& ctx) {
    TypeAST* lhsType = checkExpr(node.lhs.get(), ctx);
    if (!lhsType) return nullptr;
    
    TypeAST* rhsType = checkExpr(node.rhs.get(), ctx);
    if (!rhsType) return nullptr;
    
    // Check if LHS is assignable (lvalue)
    // For compound assignments, the LHS must be mutable
    if (node.op != AssignOp::Assign) {
        // Compound assignment: lhs = lhs op rhs
        // We'll let the binary operator check handle compatibility
        // First check if LHS is mutable
        if (auto* id = node.lhs->as<IdentifierExprAST>()) {
            Symbol* sym = ctx.symbols->lookup(id->name);
            if (sym && sym->declKw == DeclKeyword::Const) {
                ctx.error(node.loc, DiagCode::E2004, "cannot assign to const variable");
                return nullptr;
            }
        }
    }
    
    // Check assignability
    if (!TypeChecker::isAssignable(rhsType, lhsType, ctx)) {
        // Try to find a `from` conversion
        Symbol* fromCast = TypeChecker::isFromCastable(rhsType, lhsType, ctx);
        if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
            // Create explicit cast node
            auto targetTypeNode = ctx.arena.make<NamedTypeAST>(
                lhsType->as<NamedTypeAST>()->name);
            targetTypeNode->loc = node.rhs->loc;

            checkExpr(node.rhs.get(), ctx);
        } else {
            ctx.error(node.loc, DiagCode::E2008,
                      "cannot assign '", LucDebug::formatType(rhsType, ctx.pool),
                      "' to '", LucDebug::formatType(lhsType, ctx.pool), "'");
            return nullptr;
        }
    }
    
    // Assignment expressions evaluate to the RHS value
    node.resolvedType = rhsType;
    node.isConst = false; // Assignment result is never const
    return rhsType;
}