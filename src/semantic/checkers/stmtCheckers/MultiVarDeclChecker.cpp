/**
 * @file MultiVarDeclChecker.cpp
 * @brief Semantic checking for multi-variable declarations.
 */

#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "semantic/checkers/declCheckers/DeclHelpers.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Multi-Variable Declaration (let a int, b string = f())
// ─────────────────────────────────────────────────────────────────────────────

void checkMultiVarDecl(MultiVarDeclAST& node, SemanticContext& ctx) {
    if (node.vars.empty()) {
        ctx.error(node.loc, DiagCode::E2002, "multi-variable declaration with no variables");
        return;
    }

    // Check RHS expression
    TypeAST* rhsType = checkExpr(node.rhs.get(), ctx);
    if (!rhsType) return;

    // Handle single variable case
    if (node.vars.size() == 1) {
        const auto& var = node.vars[0];
        TypeAST* varType = ctx.dispatcher->resolveType(var.second.get());
        if (!varType) return;

        // Check assignability with possible from-casting
        if (!TypeChecker::isAssignable(rhsType, varType, ctx)) {
            // Try to find a `from` conversion
            Symbol* fromCast = TypeChecker::isFromCastable(rhsType, varType, ctx);
            if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
                // Rewrite the RHS as an explicit cast
                auto targetTypeNode = ctx.arena.make<NamedTypeAST>(
                    varType->as<NamedTypeAST>()->name);
                targetTypeNode->loc = node.rhs->loc;
                auto convExpr = ctx.arena.make<TypeConvExprAST>(
                    std::move(targetTypeNode), std::move(node.rhs), false);
                convExpr->loc = node.rhs->loc;
                node.rhs = std::move(convExpr);
                checkExpr(node.rhs.get(), ctx);
            } else {
                ctx.error(node.rhs->loc, DiagCode::E2008,
                          "cannot initialize '", LucDebug::formatType(varType, ctx.pool),
                          "' from '", LucDebug::formatType(rhsType, ctx.pool),
                          "' for variable '", ctx.pool.lookup(var.first), "'");
                return;
            }
        }

        // const requires compile-time constant
        if (node.keyword == DeclKeyword::Const && !isConstExpr(node.rhs.get(), ctx)) {
            ctx.error(node.loc, DiagCode::E2031,
                      "const '", ctx.pool.lookup(var.first),
                      "' initialiser must be a compile‑time constant expression");
        }

        // nil assignment to non-nullable type
        if (node.rhs->isa<LiteralExprAST>()) {
            auto* lit = node.rhs->as<LiteralExprAST>();
            if (lit->kind == LiteralKind::Nil && !TypeChecker::isNullable(varType, ctx)) {
                ctx.error(node.loc, DiagCode::E2032,
                          "nil cannot be assigned to non-nullable type '",
                          LucDebug::formatType(varType, ctx.pool), "'");
                return;
            }
        }

        // Declare the variable
        Symbol varSym;
        varSym.name = var.first;
        varSym.kind = SymbolKind::Var;
        varSym.declKw = node.keyword;
        varSym.visibility = Visibility::Private;
        varSym.type = varType;
        varSym.decl = &node;
        varSym.loc = node.loc;

        if (!ctx.symbols->declare(varSym)) {
            ctx.error(node.loc, DiagCode::E2005,
                      "duplicate declaration of variable '", ctx.pool.lookup(var.first), "'");
        }
        return;
    }

    // Multi-variable declaration with multiple variables
    // Need to get multiple return types from RHS (must be a function call)
    if (auto* callExpr = node.rhs->as<CallExprAST>()) {
        TypeAST* calleeType = callExpr->callee->resolvedType;
        if (calleeType && calleeType->isa<FuncTypeAST>()) {
            auto* funcType = calleeType->as<FuncTypeAST>();
            const auto& returnTypes = funcType->sig.returnTypes;

            if (returnTypes.size() != node.vars.size()) {
                ctx.error(node.rhs->loc, DiagCode::E2028,
                          "multi-variable declaration value count mismatch: expected ",
                          returnTypes.size(), " values, got ", node.vars.size());
                return;
            }

            // Declare each variable and check assignability
            for (size_t i = 0; i < node.vars.size(); ++i) {
                const auto& var = node.vars[i];
                TypeAST* retType = returnTypes[i].get();
                TypeAST* varType = ctx.dispatcher->resolveType(var.second.get());
                if (!varType) continue;

                if (!TypeChecker::isAssignable(retType, varType, ctx)) {
                    ctx.error(node.rhs->loc, DiagCode::E2008,
                              "cannot initialize variable '", ctx.pool.lookup(var.first),
                              "' of type '", LucDebug::formatType(varType, ctx.pool),
                              "' from return value of type '",
                              LucDebug::formatType(retType, ctx.pool), "'");
                    continue;
                }

                // const requires compile-time constant for multi-var decl as well
                if (node.keyword == DeclKeyword::Const && !isConstExpr(node.rhs.get(), ctx)) {
                    ctx.error(node.loc, DiagCode::E2031,
                              "const '", ctx.pool.lookup(var.first),
                              "' initialiser must be a compile‑time constant expression");
                }

                // Declare the variable
                Symbol varSym;
                varSym.name = var.first;
                varSym.kind = SymbolKind::Var;
                varSym.declKw = node.keyword;
                varSym.visibility = Visibility::Private;
                varSym.type = varType;
                varSym.decl = &node;
                varSym.loc = node.loc;

                if (!ctx.symbols->declare(varSym)) {
                    ctx.error(node.loc, DiagCode::E2005,
                              "duplicate declaration of variable '", ctx.pool.lookup(var.first), "'");
                }
            }
            return;
        }
    }

    ctx.error(node.loc, DiagCode::E2002,
              "multi-variable declaration with multiple variables requires "
              "a function call returning multiple values");
}