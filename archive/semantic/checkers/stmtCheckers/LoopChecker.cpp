/**
 * @file LoopChecker.cpp
 * @brief Semantic checking for loop statements: for, while, do-while.
 */

#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// Helper: get element type from iterable
static TypeAST* getIterableElementType(TypeAST* iterableType, SemanticContext& ctx) {
    if (!iterableType) return nullptr;
    
    if (iterableType->isa<ArrayTypeAST>()) {
        return iterableType->as<ArrayTypeAST>()->element.get();
    } else if (iterableType->kind == ASTKind::RangeExpr) {
        return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int).release();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// While Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkWhileStmt(WhileStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (!condType) return;

    if (!TypeChecker::isBooleanCompatible(condType, ctx)) {
        ctx.error(node.condition->loc, DiagCode::E2002, "while condition must be boolean");
        return;
    }

    ctx.enterLoop();
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    }
    ctx.exitLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// For Statement (range or collection iteration)
// ─────────────────────────────────────────────────────────────────────────────

void checkForStmt(ForStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    TypeAST* iterableType = checkExpr(node.iterable.get(), ctx);
    if (!iterableType) return;

    // Check for nullable iterable
    if (TypeChecker::isNullable(iterableType, ctx)) {
        ctx.error(node.iterable->loc, DiagCode::E2002, "for loop iterable cannot be nullable");
        return;
    }

    TypeAST* elementType = getIterableElementType(iterableType, ctx);
    if (!elementType) {
        ctx.error(node.iterable->loc, DiagCode::E2002,
                  "for loop iterable must be an array, slice, dynamic array, or range");
        return;
    }

    // Determine iteration variable type
    TypeAST* varType = elementType;
    if (node.iterVar && node.iterVar->type) {
        varType = ctx.dispatcher->resolveType(node.iterVar->type.get());
        if (!varType) return;
        
        if (!TypeChecker::isAssignable(elementType, varType, ctx)) {
            ctx.error(node.iterVar->loc, DiagCode::E2002,
                      "iteration variable type mismatch: expected ",
                      LucDebug::kindToString(elementType->kind), ", got ",
                      LucDebug::kindToString(varType->kind));
            return;
        }
    }

    // Push scope and declare iteration variable
    ctx.symbols->pushScope();

    if (node.iterVar) {
        Symbol varSym;
        varSym.name = node.iterVar->name;
        varSym.kind = SymbolKind::Var;
        varSym.declKw = DeclKeyword::Let;
        varSym.visibility = Visibility::Private;
        varSym.type = varType;
        varSym.decl = node.iterVar.get();
        varSym.loc = node.iterVar->loc;
        
        if (!ctx.symbols->declare(varSym)) {
            ctx.error(node.iterVar->loc, DiagCode::E2005,
                      "duplicate declaration of loop variable '",
                      ctx.pool.lookup(node.iterVar->name), "'");
        }
    }

    // Check step expression (if present, must be integer)
    if (node.step) {
        TypeAST* stepType = checkExpr(node.step.get(), ctx);
        if (stepType && !TypeChecker::isIntegerType(stepType, ctx)) {
            ctx.error(node.step->loc, DiagCode::E2002, "step expression must be integer");
        }
    }

    // Check body
    ctx.enterLoop();
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    }
    ctx.exitLoop();

    ctx.symbols->popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// Do-While Statement
// ─────────────────────────────────────────────────────────────────────────────

void checkDoWhileStmt(DoWhileStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    ctx.enterLoop();
    
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    }
    
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (condType && !TypeChecker::isBooleanCompatible(condType, ctx)) {
        ctx.error(node.condition->loc, DiagCode::E2002, "do-while condition must be boolean");
    }
    
    ctx.exitLoop();
}