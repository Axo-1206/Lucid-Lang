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

#include "SemanticContext.hpp"
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
// Expression Checker
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Recursively checks an expression, sets node->resolvedType, and returns it.
 * @param node Expression to check (may be null)
 * @param ctx  Semantic context
 * @return Resolved TypeAST* (or nullptr on error)
 */
TypeAST* checkExpr(ExprAST* node, SemanticContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Statement Checker
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Recursively checks a statement, managing scopes and control flow.
 * @param node           Statement to check (may be null)
 * @param ctx            Semantic context
 * @param expectedReturn Expected return type of the enclosing function
 *                       (nullptr for void functions)
 */
void checkStmt(StmtAST* node, SemanticContext& ctx, TypeAST* expectedReturn = nullptr);