/**
 * @file SemanticChecker.hpp
 * @brief Declarations for all semantic checking functions (Phase 3).
 *
 * This header provides the unified interface for declaration, expression,
 * and statement checking. All functions accept a SemanticContext that
 * bundles symbol table, type resolver, diagnostic engine, and depth counters.
 *
 * Declaration checkers take an optional `isLocal` flag to distinguish
 * top‑level declarations (false) from local declarations inside blocks (true).
 * Local declarations reject visibility modifiers (pub/export) and may have
 * additional scoping restrictions.
 *
 * Statement checkers take an `expectedReturn` type (nullptr for void functions)
 * to validate return statements.
 */

#pragma once

#include "semantic/helpers/SemanticContext.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Declaration Checkers
// ─────────────────────────────────────────────────────────────────────────────

void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx);

void checkVarDecl(VarDeclAST& node, SemanticContext& ctx, bool isLocal = false);
void checkFuncDecl(FuncDeclAST& node, SemanticContext& ctx, bool isLocal = false);
void checkStructDecl(StructDeclAST& node, SemanticContext& ctx, bool isLocal = false);
void checkEnumDecl(EnumDeclAST& node, SemanticContext& ctx, bool isLocal = false);
void checkTraitDecl(TraitDeclAST& node, SemanticContext& ctx, bool isLocal = false);
void checkImplDecl(ImplDeclAST& node, SemanticContext& ctx, bool isLocal = false);
void checkFromDecl(FromDeclAST& node, SemanticContext& ctx, bool isLocal = false);

// ─────────────────────────────────────────────────────────────────────────────
// Statement Checkers (dispatched by checkStmt)
// ─────────────────────────────────────────────────────────────────────────────

void checkStmt(StmtAST* node, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);

// Block and declarations
void checkBlockStmt(BlockStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkDeclStmt(DeclStmtAST& node, SemanticContext& ctx);
void checkExprStmt(ExprStmtAST& node, SemanticContext& ctx);

// Control flow (if, return, break, continue)
void checkIfStmt(IfStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkReturnStmt(ReturnStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkBreakStmt(BreakStmtAST& node, SemanticContext& ctx);
void checkContinueStmt(ContinueStmtAST& node, SemanticContext& ctx);

// Loops
void checkWhileStmt(WhileStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkForStmt(ForStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkDoWhileStmt(DoWhileStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);

// Switch and multi-assignment
void checkSwitchStmt(SwitchStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkMultiVarDecl(MultiVarDeclAST& node, SemanticContext& ctx);
void checkMultiAssignStmt(MultiAssignStmtAST& node, SemanticContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Expression Checker
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx);