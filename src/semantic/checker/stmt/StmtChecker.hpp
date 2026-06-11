/**
 * @file StmtChecker.hpp
 * @brief Statement semantic validation and control flow checking.
 * 
 * ============================================================================
 * STATEMENT CHECKER
 * ============================================================================
 * 
 * This module validates statements and their control flow semantics.
 * 
 * ─── Dispatch Pattern ──────────────────────────────────────────────────────
 * 
 *   checkStmt(stmt, ctx) → dispatches to specific checker based on kind
 *   Each checker may push/pop scopes and validate control flow
 * 
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * 
 *   - ExprChecker: for expression validation
 *   - TypeChecker: for type compatibility
 *   - ScopeStack: for scope management
 * 
 * ─── Statement Categories ─────────────────────────────────────────────────
 * 
 *   Block:        { stmt* } – creates new scope
 *   Declaration:  local var/func declarations
 *   Branching:    if, switch
 *   Loops:        for, while, do-while
 *   Jump:         return, break, continue
 *   Expression:   expression as statement (value discarded)
 * 
 * ─── Control Flow Context ─────────────────────────────────────────────────
 * 
 *   - loopDepth: tracks nesting for break/continue validation
 *   - expectedReturn: the function's return type (for return statements)
 * 
 * @see ExprChecker for expression validation
 * @see TypeChecker for type utilities
 */

#pragma once

#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/scope/ScopeStack.hpp"

// ============================================================================
// Dispatcher
// ============================================================================

/**
 * @brief Main entry point for statement checking.
 * 
 * Dispatches to the appropriate checker based on statement kind.
 * 
 * @param stmt The statement to check
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type (for return statements)
 */
void checkStmt(StmtAST* stmt, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);

// ============================================================================
// Block Statements
// ============================================================================

/**
 * @brief Checks a block statement.
 * 
 * Pushes a new scope, checks all statements in order, then pops the scope.
 * 
 * @param block The block statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type
 */
void checkBlockStmt(BlockStmtAST* block, SemanticContext& ctx, TypeAST* expectedReturn);

// ============================================================================
// Declaration Statements
// ============================================================================

/**
 * @brief Checks a declaration statement (local var/func).
 * 
 * @param declStmt The declaration statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type (unused)
 */
void checkDeclStmt(DeclStmtAST* declStmt, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);

// ============================================================================
// Expression Statements
// ============================================================================

/**
 * @brief Checks an expression statement (expression with discarded value).
 * 
 * @param exprStmt The expression statement
 * @param ctx Semantic context
 * @param expectedReturn Unused
 */
void checkExprStmt(ExprStmtAST* exprStmt, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);

// ============================================================================
// Branching Statements
// ============================================================================

/**
 * @brief Checks an if statement.
 * 
 * Condition must be boolean. Then/then branches are checked.
 * 
 * @param ifStmt The if statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type
 */
void checkIfStmt(IfStmtAST* ifStmt, SemanticContext& ctx, TypeAST* expectedReturn);

/**
 * @brief Checks a switch statement.
 * 
 * Subject must be of a type that supports equality comparison.
 * Case values must be constant and of the same type as the subject.
 * 
 * @param switchStmt The switch statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type
 */
void checkSwitchStmt(SwitchStmtAST* switchStmt, SemanticContext& ctx, TypeAST* expectedReturn);

// ============================================================================
// Loop Statements
// ============================================================================

/**
 * @brief Checks a for loop statement.
 * 
 * @param forStmt The for statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type
 */
void checkForStmt(ForStmtAST* forStmt, SemanticContext& ctx, TypeAST* expectedReturn);

/**
 * @brief Checks a while loop statement.
 * 
 * Condition must be boolean.
 * 
 * @param whileStmt The while statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type
 */
void checkWhileStmt(WhileStmtAST* whileStmt, SemanticContext& ctx, TypeAST* expectedReturn);

/**
 * @brief Checks a do-while loop statement.
 * 
 * Body executes at least once; condition checked after.
 * 
 * @param doWhileStmt The do-while statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type
 */
void checkDoWhileStmt(DoWhileStmtAST* doWhileStmt, SemanticContext& ctx, TypeAST* expectedReturn);

// ============================================================================
// Jump Statements
// ============================================================================

/**
 * @brief Checks a return statement.
 * 
 * Values must match the function's expected return type.
 * 
 * @param retStmt The return statement
 * @param ctx Semantic context
 * @param expectedReturn The function's expected return type (may be nullptr for void)
 */
void checkReturnStmt(ReturnStmtAST* retStmt, SemanticContext& ctx, TypeAST* expectedReturn);

/**
 * @brief Checks a break statement.
 * 
 * Must be inside a loop (loopDepth > 0).
 * 
 * @param breakStmt The break statement
 * @param ctx Semantic context
 */
void checkBreakStmt(BreakStmtAST* breakStmt, SemanticContext& ctx);

/**
 * @brief Checks a continue statement.
 * 
 * Must be inside a loop (loopDepth > 0).
 * 
 * @param continueStmt The continue statement
 * @param ctx Semantic context
 */
void checkContinueStmt(ContinueStmtAST* continueStmt, SemanticContext& ctx);

// ============================================================================
// Multi-Variable Declarations
// ============================================================================

/**
 * @brief Checks a multi-variable declaration (let x int, y int = f()).
 * 
 * @param multiDecl The multi-variable declaration
 * @param ctx Semantic context
 * @param expectedReturn Unused
 */
void checkMultiVarDecl(MultiVarDeclAST* multiDecl, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);

/**
 * @brief Checks a multi-assignment statement (x, y = f()).
 * 
 * @param multiAssign The multi-assignment statement
 * @param ctx Semantic context
 * @param expectedReturn Unused
 */
void checkMultiAssignStmt(MultiAssignStmtAST* multiAssign, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);
