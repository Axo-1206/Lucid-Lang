/**
 * @file SwitchChecker.cpp
 * @brief Semantic checking for switch statements.
 */

#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// Switch Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkSwitchStmt(SwitchStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* subjectType = checkExpr(node.subject.get(), ctx);
    if (!subjectType) return;

    // Value comparability required for case values
    if (!TypeChecker::isValueComparable(subjectType, ctx)) {
        ctx.error(node.subject->loc, DiagCode::E2002,
                  "switch subject type does not support value equality");
        return;
    }

    // Track seen case values to detect duplicates
    std::unordered_set<int64_t> seenIntValues;
    std::unordered_set<std::string> seenStringValues;
    bool hasDefault = (node.defaultBody != nullptr);

    // Check each case
    for (auto& caseNode : node.cases) {
        if (!caseNode) continue;

        // Check each case value (must be constant and comparable)
        for (auto& valExpr : caseNode->values) {
            TypeAST* valType = checkExpr(valExpr.get(), ctx);
            if (!valType) continue;

            if (!TypeChecker::isAssignable(valType, subjectType, ctx)) {
                ctx.error(valExpr->loc, DiagCode::E2002, "case value type mismatch");
                continue;
            }

            // Check for duplicate case values
            int64_t intVal;
            if (TypeChecker::getConstantIntValue(valExpr.get(), intVal, ctx)) {
                if (!seenIntValues.insert(intVal).second) {
                    ctx.error(valExpr->loc, DiagCode::E2005,
                              "duplicate case value: ", intVal);
                }
            } else if (valExpr->isa<LiteralExprAST>()) {
                auto* lit = valExpr->as<LiteralExprAST>();
                if (lit->kind == LiteralKind::String) {
                    std::string strVal = std::string(ctx.pool.lookup(lit->value));
                    if (!seenStringValues.insert(strVal).second) {
                        ctx.error(valExpr->loc, DiagCode::E2005,
                                  "duplicate case value: '", strVal, "'");
                    }
                }
            }
        }

        // Check the case body
        if (caseNode->body) {
            checkStmt(caseNode->body.get(), ctx, expectedReturn);
        }
    }

    // No default case warning (optional)
    if (!hasDefault && !node.cases.empty()) {
        ctx.warning(node.loc, DiagCode::W6009,
                    "switch statement has no default case");
    }

    // Check default body if present
    if (node.defaultBody) {
        checkStmt(node.defaultBody.get(), ctx, expectedReturn);
    }
}