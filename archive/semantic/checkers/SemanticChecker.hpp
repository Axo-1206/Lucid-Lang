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
 *
 * Expression checkers are organised by category (literal, operator, special, other, match)
 * and each returns the resolved TypeAST* for the expression.
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
// Statement Checkers
// ─────────────────────────────────────────────────────────────────────────────

void checkStmt(StmtAST* node, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);

// Block and declarations
void checkBlockStmt(BlockStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn);
void checkDeclStmt(DeclStmtAST& node, SemanticContext& ctx);
void checkExprStmt(ExprStmtAST& node, SemanticContext& ctx);

// Control flow
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
// Expression Checkers (dispatched by checkExpr)
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx);

// ── Literal Expressions ─────────────────────────────────────────────────────
TypeAST* checkLiteralExpr(LiteralExprAST& node, SemanticContext& ctx);
TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST& node, SemanticContext& ctx);
TypeAST* checkStructLiteralExpr(StructLiteralExprAST& node, SemanticContext& ctx);
TypeAST* checkAnonFuncExpr(AnonFuncExprAST& node, SemanticContext& ctx);

// ── Operator Expressions ────────────────────────────────────────────────────
TypeAST* checkBinaryExpr(BinaryExprAST& node, SemanticContext& ctx);
TypeAST* checkUnaryExpr(UnaryExprAST& node, SemanticContext& ctx);
TypeAST* checkAssignExpr(AssignExprAST& node, SemanticContext& ctx);
TypeAST* checkIsExpr(IsExprAST& node, SemanticContext& ctx);
TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST& node, SemanticContext& ctx);
TypeAST* checkPipelineExpr(PipelineExprAST& node, SemanticContext& ctx);
TypeAST* checkComposeExpr(ComposeExprAST& node, SemanticContext& ctx);

// ── Special Expressions ─────────────────────────────────────────────────────
TypeAST* checkAwaitExpr(AwaitExprAST& node, SemanticContext& ctx);
TypeAST* checkIfExpr(IfExprAST& node, SemanticContext& ctx);
TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST& node, SemanticContext& ctx);
TypeAST* checkRangeExpr(RangeExprAST& node, SemanticContext& ctx);
TypeAST* checkNullableChainExpr(NullableChainExprAST& node, SemanticContext& ctx);

// ── Other Expressions ───────────────────────────────────────────────────────
TypeAST* checkCallExpr(CallExprAST& node, SemanticContext& ctx);
TypeAST* checkIndexExpr(IndexExprAST& node, SemanticContext& ctx);
TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SemanticContext& ctx);
TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST& node, SemanticContext& ctx);
TypeAST* checkIdentifierExpr(IdentifierExprAST& node, SemanticContext& ctx);

// ── Match Expressions ───────────────────────────────────────────────────────
TypeAST* checkMatchExpr(MatchExprAST& node, SemanticContext& ctx);