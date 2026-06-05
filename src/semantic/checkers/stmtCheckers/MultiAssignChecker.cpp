/**
 * @file MultiAssignChecker.cpp
 * @brief Semantic checking for multi-assignment statements.
 */

#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Multi-Assignment Statement (a, b = f())
// ─────────────────────────────────────────────────────────────────────────────

void checkMultiAssignStmt(MultiAssignStmtAST& node, SemanticContext& ctx) {
    if (node.lhs.empty()) {
        ctx.error(node.loc, DiagCode::E2002, "multi-assignment with no left-hand side");
        return;
    }

    // Check RHS expression
    TypeAST* rhsType = checkExpr(node.rhs.get(), ctx);
    if (!rhsType) return;

    // For multi-return, we need to get the return types from the RHS call
    // For now, handle single assignment case
    if (node.lhs.size() == 1) {
        // Single assignment: check assignability from RHS type to LHS type
        TypeAST* lhsType = checkExpr(node.lhs[0].get(), ctx);
        if (!lhsType) return;

        // Check if LHS is assignable (lvalue)
        if (!node.lhs[0]->isa<IdentifierExprAST>() &&
            !node.lhs[0]->isa<FieldAccessExprAST>() &&
            !node.lhs[0]->isa<IndexExprAST>()) {
            ctx.error(node.lhs[0]->loc, DiagCode::E2029,
                      "assignment target is not an lvalue");
            return;
        }

        // Check assignability with possible from-casting
        if (!TypeChecker::isAssignable(rhsType, lhsType, ctx)) {
            // Try to find a `from` conversion
            Symbol* fromCast = TypeChecker::isFromCastable(rhsType, lhsType, ctx);
            if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
                // Rewrite the RHS as an explicit cast (similar to var decl)
                auto targetTypeNode = ctx.arena.make<NamedTypeAST>(
                    lhsType->as<NamedTypeAST>()->name);
                targetTypeNode->loc = node.rhs->loc;
                auto convExpr = ctx.arena.make<TypeConvExprAST>(
                    std::move(targetTypeNode), std::move(node.rhs), false);
                convExpr->loc = node.rhs->loc;
                node.rhs = std::move(convExpr);
                checkExpr(node.rhs.get(), ctx);
            } else {
                ctx.error(node.rhs->loc, DiagCode::E2008,
                          "cannot assign to '", LucDebug::formatType(lhsType, ctx.pool),
                          "' from '", LucDebug::formatType(rhsType, ctx.pool), "'");
            }
        }
    } else {
        // Multi-assignment with multiple LHS
        // Need to get multiple return types from RHS (must be a function call)
        if (auto* callExpr = node.rhs->as<CallExprAST>()) {
            // Get the callee's return types
            TypeAST* calleeType = callExpr->callee->resolvedType;
            if (calleeType && calleeType->isa<FuncTypeAST>()) {
                auto* funcType = calleeType->as<FuncTypeAST>();
                const auto& returnTypes = funcType->sig.returnTypes;

                if (returnTypes.size() != node.lhs.size()) {
                    ctx.error(node.rhs->loc, DiagCode::E2028,
                              "multi-assignment value count mismatch: expected ",
                              returnTypes.size(), " values, got ", node.lhs.size());
                    return;
                }

                // Check each LHS against corresponding return type
                for (size_t i = 0; i < node.lhs.size(); ++i) {
                    TypeAST* lhsType = checkExpr(node.lhs[i].get(), ctx);
                    if (!lhsType) continue;

                    // Check if LHS is assignable (lvalue)
                    if (!node.lhs[i]->isa<IdentifierExprAST>() &&
                        !node.lhs[i]->isa<FieldAccessExprAST>() &&
                        !node.lhs[i]->isa<IndexExprAST>()) {
                        ctx.error(node.lhs[i]->loc, DiagCode::E2029,
                                  "assignment target is not an lvalue");
                        continue;
                    }

                    TypeAST* retType = returnTypes[i].get();
                    if (!TypeChecker::isAssignable(retType, lhsType, ctx)) {
                        ctx.error(node.rhs->loc, DiagCode::E2008,
                                  "cannot assign value ", i, " to '",
                                  LucDebug::formatType(lhsType, ctx.pool), "' from '",
                                  LucDebug::formatType(retType, ctx.pool), "'");
                    }
                }
                return;
            }
        }

        ctx.error(node.loc, DiagCode::E2002,
                  "multi-assignment with more than one left-hand side requires "
                  "a function call returning multiple values");
    }
}