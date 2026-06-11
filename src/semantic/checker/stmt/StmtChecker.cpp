/**
 * @file StmtChecker.cpp
 * @brief Implementation of statement checkers.
 */

#include "StmtChecker.hpp"
#include "checker/ExprChecker.hpp"
#include "semantic/checker/TypeChecker.hpp"
#include "semantic/resolver/TypeResolver.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace luc::checker {

// ============================================================================
// Dispatcher
// ============================================================================

void checkStmt(StmtAST* stmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    if (!stmt) return;
    
    LUC_LOG_SEMANTIC_EXTREME("checkStmt: kind=" << LucDebug::kindToString(stmt->kind));
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            checkBlockStmt(stmt->as<BlockStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::DeclStmt:
            checkDeclStmt(stmt->as<DeclStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ExprStmt:
            checkExprStmt(stmt->as<ExprStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::IfStmt:
            checkIfStmt(stmt->as<IfStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::SwitchStmt:
            checkSwitchStmt(stmt->as<SwitchStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ForStmt:
            checkForStmt(stmt->as<ForStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::WhileStmt:
            checkWhileStmt(stmt->as<WhileStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::DoWhileStmt:
            checkDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::ReturnStmt:
            checkReturnStmt(stmt->as<ReturnStmtAST>(), ctx, expectedReturn);
            break;
        case ASTKind::BreakStmt:
            checkBreakStmt(stmt->as<BreakStmtAST>(), ctx);
            break;
        case ASTKind::ContinueStmt:
            checkContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
            break;
        case ASTKind::MultiVarDecl:
            checkMultiVarDecl(stmt->as<MultiVarDeclAST>(), ctx, expectedReturn);
            break;
        case ASTKind::MultiAssignStmt:
            checkMultiAssignStmt(stmt->as<MultiAssignStmtAST>(), ctx, expectedReturn);
            break;
        default:
            LUC_LOG_SEMANTIC("checkStmt: unhandled statement kind: "
                             << LucDebug::kindToString(stmt->kind));
            break;
    }
}

// ============================================================================
// Block Statements
// ============================================================================

void checkBlockStmt(BlockStmtAST* block, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkBlockStmt: " << block->stmts.size() << " statements");
    
    // Push a new scope for the block
    ctx.scope.push();
    
    // Check all statements in order
    for (auto* stmt : block->stmts) {
        checkStmt(stmt, ctx, expectedReturn);
        
        // If we hit a return/break/continue that terminates the block,
        // we could stop checking, but continue for now to find more errors
    }
    
    // Pop the scope
    ctx.scope.pop();
}

// ============================================================================
// Declaration Statements
// ============================================================================

void checkDeclStmt(DeclStmtAST* declStmt, SemanticContext& ctx, TypeAST* /*expectedReturn*/) {
    LUC_LOG_SEMANTIC_EXTREME("checkDeclStmt");
    
    if (!declStmt->decl) return;
    
    // Local declarations are registered in the current scope by the collector
    // Here we just need to check initializers
    if (auto* var = declStmt->decl->as<VarDeclAST>()) {
        if (var->init) {
            TypeAST* initType = checkExpr(var->init, ctx);
            if (initType && var->valueType) {
                if (!TypeChecker::isAssignable(initType, var->valueType, ctx)) {
                    ctx.error(var->init->loc, DiagCode::E2001,
                              "cannot initialize variable '", ctx.pool.lookup(var->name),
                              "' with value of different type");
                }
            }
        } else if (var->keyword == DeclKeyword::Const) {
            ctx.error(var->loc, DiagCode::E2001,
                      "const variable '", ctx.pool.lookup(var->name),
                      "' must be initialized");
        } else if (!TypeChecker::isNullable(var->valueType, *ctx.typeResolver)) {
            ctx.error(var->loc, DiagCode::E2001,
                      "non-nullable variable '", ctx.pool.lookup(var->name),
                      "' must be initialized");
        }
    }
    // FuncDeclAST local functions are checked elsewhere
}

// ============================================================================
// Expression Statements
// ============================================================================

void checkExprStmt(ExprStmtAST* exprStmt, SemanticContext& ctx, TypeAST* /*expectedReturn*/) {
    LUC_LOG_SEMANTIC_EXTREME("checkExprStmt");
    
    TypeAST* resultType = checkExpr(exprStmt->expr, ctx);
    
    // Warning for discarded non-void result (except for function calls)
    if (resultType && !TypeChecker::isVoid(resultType)) {
        // Don't warn for void function calls or assignments
        if (!exprStmt->expr->isa<CallExprAST>() &&
            !exprStmt->expr->isa<AssignExprAST>()) {
            ctx.warning(exprStmt->loc, DiagCode::W6001,
                        "expression result discarded");
        }
    }
}

// ============================================================================
// Branching Statements
// ============================================================================

void checkIfStmt(IfStmtAST* ifStmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkIfStmt");
    
    // Check condition
    TypeAST* condType = checkExpr(ifStmt->condition, ctx);
    if (condType) {
        condType = TypeChecker::getUnderlyingType(condType, *ctx.typeResolver);
        if (!TypeChecker::isBoolean(condType, *ctx.typeResolver)) {
            ctx.error(ifStmt->condition->loc, DiagCode::E2001,
                      "if condition must be boolean");
        }
    }
    
    // Check then branch
    checkStmt(ifStmt->thenBranch, ctx, expectedReturn);
    
    // Check else branch if present
    if (ifStmt->elseBranch) {
        checkStmt(ifStmt->elseBranch, ctx, expectedReturn);
    }
}

void checkSwitchStmt(SwitchStmtAST* switchStmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkSwitchStmt: " << switchStmt->cases.size() << " cases");
    
    // Check subject
    TypeAST* subjectType = checkExpr(switchStmt->subject, ctx);
    if (!subjectType) return;
    
    subjectType = TypeChecker::getUnderlyingType(subjectType, *ctx.typeResolver);
    
    // Check each case
    for (auto* caseClause : switchStmt->cases) {
        // Check case values
        for (auto* value : caseClause->values) {
            TypeAST* valueType = checkExpr(value, ctx);
            if (valueType) {
                valueType = TypeChecker::getUnderlyingType(valueType, *ctx.typeResolver);
                if (!TypeChecker::isEqual(subjectType, valueType, *ctx.typeResolver)) {
                    ctx.error(value->loc, DiagCode::E2001,
                              "case value type does not match switch subject");
                }
                
                // Check if value is constant
                if (!value->isConst) {
                    ctx.error(value->loc, DiagCode::E2001,
                              "case value must be constant");
                }
            }
        }
        
        // Check case body
        checkStmt(caseClause->body, ctx, expectedReturn);
    }
    
    // Check default body if present
    if (switchStmt->defaultBody) {
        checkStmt(switchStmt->defaultBody, ctx, expectedReturn);
    }
}

// ============================================================================
// Loop Statements
// ============================================================================

void checkForStmt(ForStmtAST* forStmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkForStmt: iterVar=" << ctx.pool.lookup(forStmt->iterVar->name));
    
    // Push scope for the iteration variable
    ctx.scope.push();
    
    // The iteration variable is already declared in the scope by the collector
    
    // Check iterable expression
    TypeAST* iterableType = checkExpr(forStmt->iterable, ctx);
    if (iterableType) {
        iterableType = TypeChecker::getUnderlyingType(iterableType, *ctx.typeResolver);
        
        // For range loops, iterable should be a range (which we treat as int)
        if (forStmt->iterable->isa<RangeExprAST>()) {
            if (!TypeChecker::isInteger(forStmt->iterVar->type, *ctx.typeResolver)) {
                ctx.error(forStmt->iterVar->loc, DiagCode::E2001,
                          "range iteration variable must be integer");
            }
        } else {
            // Collection iteration
            if (!TypeChecker::isArray(iterableType, *ctx.typeResolver)) {
                ctx.error(forStmt->iterable->loc, DiagCode::E2001,
                          "for loop iterable must be an array or range");
            }
            
            // Check that iterVar type matches element type
            TypeAST* elemType = TypeChecker::getElementType(iterableType, *ctx.typeResolver);
            if (elemType && forStmt->iterVar->type) {
                if (!TypeChecker::isAssignable(elemType, forStmt->iterVar->type, ctx)) {
                    ctx.error(forStmt->iterVar->loc, DiagCode::E2001,
                              "iteration variable type does not match array element type");
                }
            }
        }
    }
    
    // Check step expression if present
    if (forStmt->step) {
        TypeAST* stepType = checkExpr(forStmt->step, ctx);
        if (stepType) {
            stepType = TypeChecker::getUnderlyingType(stepType, *ctx.typeResolver);
            if (!TypeChecker::isInteger(stepType, *ctx.typeResolver)) {
                ctx.error(forStmt->step->loc, DiagCode::E2001,
                          "step value must be integer");
            }
        }
    }
    
    // Check loop body (with loop depth tracking)
    ctx.enterLoop();
    checkStmt(forStmt->body, ctx, expectedReturn);
    ctx.exitLoop();
    
    ctx.scope.pop();
}

void checkWhileStmt(WhileStmtAST* whileStmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkWhileStmt");
    
    // Check condition
    TypeAST* condType = checkExpr(whileStmt->condition, ctx);
    if (condType) {
        condType = TypeChecker::getUnderlyingType(condType, *ctx.typeResolver);
        if (!TypeChecker::isBoolean(condType, *ctx.typeResolver)) {
            ctx.error(whileStmt->condition->loc, DiagCode::E2001,
                      "while condition must be boolean");
        }
    }
    
    // Check loop body (with loop depth tracking)
    ctx.enterLoop();
    checkStmt(whileStmt->body, ctx, expectedReturn);
    ctx.exitLoop();
}

void checkDoWhileStmt(DoWhileStmtAST* doWhileStmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkDoWhileStmt");
    
    // Check loop body (with loop depth tracking)
    ctx.enterLoop();
    checkStmt(doWhileStmt->body, ctx, expectedReturn);
    ctx.exitLoop();
    
    // Check condition after body
    TypeAST* condType = checkExpr(doWhileStmt->condition, ctx);
    if (condType) {
        condType = TypeChecker::getUnderlyingType(condType, *ctx.typeResolver);
        if (!TypeChecker::isBoolean(condType, *ctx.typeResolver)) {
            ctx.error(doWhileStmt->condition->loc, DiagCode::E2001,
                      "do-while condition must be boolean");
        }
    }
}

// ============================================================================
// Jump Statements
// ============================================================================

void checkReturnStmt(ReturnStmtAST* retStmt, SemanticContext& ctx, TypeAST* expectedReturn) {
    LUC_LOG_SEMANTIC_EXTREME("checkReturnStmt: " << retStmt->values.size() << " values");
    
    bool isVoidReturn = retStmt->values.empty();
    bool expectsVoid = TypeChecker::isVoid(expectedReturn);
    
    if (isVoidReturn && expectsVoid) {
        // Void function returning nothing – OK
        return;
    }
    
    if (isVoidReturn && !expectsVoid) {
        ctx.error(retStmt->loc, DiagCode::E2001,
                  "non-void function must return a value");
        return;
    }
    
    if (!isVoidReturn && expectsVoid) {
        ctx.error(retStmt->loc, DiagCode::E2001,
                  "void function cannot return a value");
        return;
    }
    
    // Check return value(s) against expected return type(s)
    if (retStmt->values.size() == 1) {
        // Single return value
        TypeAST* retType = checkExpr(retStmt->values[0], ctx);
        if (retType && expectedReturn) {
            if (!TypeChecker::isAssignable(retType, expectedReturn, ctx)) {
                ctx.error(retStmt->values[0]->loc, DiagCode::E2001,
                          "return value type does not match function return type");
            }
        }
    } else {
        // Multi-return – need to check against multiple return types
        // This requires function return type to be a tuple
        // For now, error
        ctx.error(retStmt->loc, DiagCode::E2001,
                  "multi-value return not yet supported");
    }
}

void checkBreakStmt(BreakStmtAST* breakStmt, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkBreakStmt");
    
    if (ctx.loopDepth == 0) {
        ctx.error(breakStmt->loc, DiagCode::E2001,
                  "'break' statement outside of loop");
    }
}

void checkContinueStmt(ContinueStmtAST* continueStmt, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkContinueStmt");
    
    if (ctx.loopDepth == 0) {
        ctx.error(continueStmt->loc, DiagCode::E2001,
                  "'continue' statement outside of loop");
    }
}

// ============================================================================
// Multi-Variable Declarations and Assignments
// ============================================================================

void checkMultiVarDecl(MultiVarDeclAST* multiDecl, SemanticContext& ctx, TypeAST* /*expectedReturn*/) {
    LUC_LOG_SEMANTIC_EXTREME("checkMultiVarDecl: " << multiDecl->vars.size() << " variables");
    
    // Check RHS expression
    TypeAST* rhsType = checkExpr(multiDecl->rhs, ctx);
    
    if (rhsType) {
        // For multi-return, we need to check against multiple types
        // This requires the RHS to return a tuple
        // For now, assume single return
        if (multiDecl->vars.size() == 1) {
            if (!TypeChecker::isAssignable(rhsType, multiDecl->vars[0].second, ctx)) {
                ctx.error(multiDecl->rhs->loc, DiagCode::E2001,
                          "cannot initialize variable with value of different type");
            }
        } else {
            ctx.error(multiDecl->loc, DiagCode::E2001,
                      "multi-value initialization not yet supported");
        }
    }
    
    // Variables are already declared in the scope by the collector
}

void checkMultiAssignStmt(MultiAssignStmtAST* multiAssign, SemanticContext& ctx, TypeAST* /*expectedReturn*/) {
    LUC_LOG_SEMANTIC_EXTREME("checkMultiAssignStmt: " << multiAssign->lhs.size() << " targets");
    
    // Check RHS expression
    TypeAST* rhsType = checkExpr(multiAssign->rhs, ctx);
    
    if (rhsType) {
        if (multiAssign->lhs.size() == 1) {
            // Check single LHS
            TypeAST* lhsType = checkExpr(multiAssign->lhs[0], ctx);
            if (lhsType && !TypeChecker::isAssignable(rhsType, lhsType, ctx)) {
                ctx.error(multiAssign->rhs->loc, DiagCode::E2001,
                          "cannot assign value to left-hand side");
            }
        } else {
            ctx.error(multiAssign->loc, DiagCode::E2001,
                      "multi-value assignment not yet supported");
        }
    }
    
    // Check each LHS is assignable (already checked in checkExpr)
}

} // namespace luc::checker