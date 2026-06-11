/**
 * @file DeclChecker.hpp
 * @brief Declaration semantic validation for top-level and local declarations.
 * 
 * ============================================================================
 * DECLARATION CHECKER
 * ============================================================================
 * 
 * This module validates declarations and their semantics.
 * 
 * ─── Dispatch Pattern ──────────────────────────────────────────────────────
 * 
 *   checkTopLevelDecl(decl, ctx) → dispatches to specific checker based on kind
 * 
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * 
 *   - ExprChecker: for expression validation (initializers)
 *   - StmtChecker: for function body validation
 *   - TypeChecker: for type compatibility
 *   - TypeResolver: for type resolution
 * 
 * ─── Declaration Categories ───────────────────────────────────────────────
 * 
 *   Variable:     let/const declarations
 *   Function:     function declarations (including extern)
 *   Struct:       struct definitions
 *   Enum:         enum definitions
 *   Trait:        trait definitions
 *   Impl:         implementation blocks
 *   From:         conversion blocks
 *   TypeAlias:    type alias definitions
 * 
 * ─── Validation Rules ─────────────────────────────────────────────────────
 * 
 *   - Duplicate method signatures in impl blocks
 *   - Trait method fulfillment
 *   - @extern attribute validation
 *   - Const initializer requirements
 *   - Visibility rules
 * 
 * @see ExprChecker for expression validation
 * @see StmtChecker for function body validation
 * @see TypeChecker for type utilities
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/resolver/TypeResolver.hpp"

namespace luc::checker {

// ============================================================================
// Dispatcher
// ============================================================================

/**
 * @brief Main entry point for top-level declaration checking.
 * 
 * Dispatches to the appropriate checker based on declaration kind.
 * 
 * @param decl The declaration to check
 * @param ctx Semantic context
 */
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx);

// ============================================================================
// Variable Declaration Checkers
// ============================================================================

/**
 * @brief Checks a variable declaration (let or const).
 * 
 * Validates:
 *   - const variables must have initializer
 *   - non-nullable variables must have initializer
 *   - initializer type matches declared type
 * 
 * @param var The variable declaration
 * @param ctx Semantic context
 */
void checkVarDecl(VarDeclAST* var, SemanticContext& ctx);

// ============================================================================
// Function Declaration Checkers
// ============================================================================

/**
 * @brief Checks a function declaration.
 * 
 * Validates:
 *   - extern functions have no body
 *   - non-extern functions must have body
 *   - return statements match declared return type
 *   - async qualifier restrictions
 * 
 * @param func The function declaration
 * @param ctx Semantic context
 */
void checkFuncDecl(FuncDeclAST* func, SemanticContext& ctx);

// ============================================================================
// Struct Declaration Checkers
// ============================================================================

/**
 * @brief Checks a struct declaration.
 * 
 * Validates:
 *   - field types are valid
 *   - no duplicate field names
 *   - self-referential fields via references/pointers
 * 
 * @param structDecl The struct declaration
 * @param ctx Semantic context
 */
void checkStructDecl(StructDeclAST* structDecl, SemanticContext& ctx);

// ============================================================================
// Enum Declaration Checkers
// ============================================================================

/**
 * @brief Checks an enum declaration.
 * 
 * Validates:
 *   - explicit values are within range
 *   - no duplicate variant names
 *   - no duplicate explicit values
 * 
 * @param enumDecl The enum declaration
 * @param ctx Semantic context
 */
void checkEnumDecl(EnumDeclAST* enumDecl, SemanticContext& ctx);

// ============================================================================
// Trait Declaration Checkers
// ============================================================================

/**
 * @brief Checks a trait declaration.
 * 
 * Validates:
 *   - no duplicate method names
 *   - method signatures are valid
 *   - no method bodies
 * 
 * @param trait The trait declaration
 * @param ctx Semantic context
 */
void checkTraitDecl(TraitDeclAST* trait, SemanticContext& ctx);

// ============================================================================
// Impl Block Checkers
// ============================================================================

/**
 * @brief Checks an impl block.
 * 
 * Validates:
 *   - all trait methods are implemented (if trait conformance)
 *   - no duplicate method names
 *   - method signatures match trait (if applicable)
 *   - injection form validation
 * 
 * @param impl The impl declaration
 * @param ctx Semantic context
 */
void checkImplDecl(ImplDeclAST* impl, SemanticContext& ctx);

// ============================================================================
// From Block Checkers
// ============================================================================

/**
 * @brief Checks a from block (implicit conversion).
 * 
 * Validates:
 *   - conversion signatures are valid
 *   - no duplicate source types
 *   - inline entry bodies are valid
 * 
 * @param from The from declaration
 * @param ctx Semantic context
 */
void checkFromDecl(FromDeclAST* from, SemanticContext& ctx);

// ============================================================================
// Type Alias Checkers
// ============================================================================

/**
 * @brief Checks a type alias declaration.
 * 
 * Validates:
 *   - aliased type is valid
 *   - no cyclic aliases (handled by resolver)
 * 
 * @param alias The type alias declaration
 * @param ctx Semantic context
 */
void checkTypeAliasDecl(TypeAliasDeclAST* alias, SemanticContext& ctx);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Checks attributes on a declaration.
 * 
 * @param decl The declaration with attributes
 * @param ctx Semantic context
 */
void checkAttributes(DeclAST* decl, SemanticContext& ctx);

/**
 * @brief Validates that a const expression is compile-time constant.
 * 
 * @param expr The expression to check
 * @param ctx Semantic context
 * @return true if the expression is constant
 */
bool isConstExpr(ExprAST* expr, SemanticContext& ctx);

/**
 * @brief Checks for duplicate method names in an impl block.
 * 
 * @param methods The methods to check
 * @param ctx Semantic context
 * @return true if no duplicates
 */
bool checkDuplicateMethods(ArenaSpan<MethodDeclPtr> methods, SemanticContext& ctx);

/**
 * @brief Checks that all trait methods are implemented.
 * 
 * @param impl The impl declaration
 * @param ctx Semantic context
 * @return true if all methods are implemented
 */
bool checkTraitFulfillment(ImplDeclAST* impl, SemanticContext& ctx);

} // namespace luc::checker