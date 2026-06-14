/**
 * @file DeclChecker.hpp
 * @brief Declaration semantic validation - Main entry point and public interface.
 * 
 * ============================================================================
 * DECLARATION CHECKER MODULE
 * ============================================================================
 * 
 * This module provides semantic validation for all declaration types in the Luc
 * language. It is organized as a single public header with multiple implementation
 * files, each handling a specific declaration category.
 * 
 * ─── Architecture ──────────────────────────────────────────────────────────
 * 
 *   The module follows a dispatch pattern:
 *   
 *   checkTopLevelDecl() ─┬─► checkVarDecl()      (VarChecker.cpp)
 *                        ├─► checkFuncDecl()     (FuncChecker.cpp)
 *                        ├─► checkStructDecl()   (StructChecker.cpp)
 *                        ├─► checkEnumDecl()     (EnumChecker.cpp)
 *                        ├─► checkTraitDecl()    (TraitChecker.cpp)
 *                        ├─► checkImplDecl()     (ImplChecker.cpp)
 *                        ├─► checkFromDecl()     (FromChecker.cpp)
 *                        └─► checkTypeAliasDecl()(TypeAliasChecker.cpp)
 * 
 * ─── Validation Flow ───────────────────────────────────────────────────────
 * 
 *   1. Attribute validation (using AttributeRegistry)
 *   2. Declaration-specific semantic rules
 *   3. Type checking for initializers and bodies
 *   4. Mutual exclusion for conflicting attributes
 * 
 * ─── Dependencies ──────────────────────────────────────────────────────────
 * 
 *   - AttributeRegistry: Attribute metadata and validation
 *   - TypeResolver: Type resolution and equality
 *   - TypeChecker: Type compatibility and inference
 *   - ExprChecker: Expression validation
 *   - StmtChecker: Statement validation (for function bodies)
 * 
 * @see AttributeRegistry for attribute validation rules
 * @see TypeResolver for type resolution
 * @see TypeChecker for type compatibility utilities
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/resolver/TypeResolver.hpp"
#include "registry/AttributeRegistry.hpp"

// ============================================================================
// Main Dispatch
// ============================================================================

/**
 * @brief Main entry point for top-level declaration checking.
 * 
 * Dispatches to the appropriate declaration-specific checker based on the
 * AST node's kind. This is the only function that external code should call.
 * 
 * @param decl The declaration to check (must be a top-level declaration)
 * @param ctx Semantic context for diagnostics and symbol resolution
 * 
 * @note PackageDeclAST and UseDeclAST are skipped as they have no semantic rules
 * @note Unknown declarations are silently ignored (error already reported by parser)
 */
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx);

// ============================================================================
// Constant Expression Validation
// ============================================================================

/**
 * @brief Determines if an expression is a compile-time constant.
 * 
 * A constant expression is one that can be evaluated at compile time:
 *   - Literals (integers, floats, strings, chars, booleans, nil)
 *   - Binary operations where both operands are constant
 *   - Unary operations where the operand is constant
 * 
 * This is used for:
 *   - `const` variable initializers
 *   - Fixed-size array dimensions
 *   - Enum explicit values
 *   - Generic argument defaults
 * 
 * @param expr The expression to test (may be modified to cache const-ness)
 * @param ctx Semantic context for type information
 * @return true if the expression is compile-time constant
 * 
 * @note This function caches the result in ExprAST::isConst for future calls
 * @note Function calls are NEVER constant (even pure functions)
 */
bool isConstExpr(ExprAST* expr, SemanticContext& ctx);

// ============================================================================
// Variable Declaration
// ============================================================================

/**
 * @brief Validates a variable declaration (let or const).
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - `const` variables must have an initializer
 *   - `const` initializers must be constant expressions
 *   - Non-nullable variables must have an initializer
 *   - Initializer type must be assignable to declared type
 * 
 * @param var The variable declaration AST node
 * @param ctx Semantic context for diagnostics
 * 
 * @example
 *   let x int = 42        ✓ Valid
 *   const PI float = 3.14 ✓ Valid
 *   let s string?         ✓ Valid (nullable, can be nil)
 *   let t int             ✗ Error: non-nullable uninitialized
 *   const N = getValue()  ✗ Error: non-constant initializer
 */
void checkVarDecl(VarDeclAST* var, SemanticContext& ctx);

// ============================================================================
// Function Declaration
// ============================================================================

/**
 * @brief Validates a function declaration.
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - `@extern` functions cannot have a body
 *   - Non-extern functions must have a body
 *   - Function body return statements match declared return type
 *   - Async qualifier restrictions (must be called with await)
 * 
 * @param func The function declaration AST node
 * @param ctx Semantic context for diagnostics and scope
 * 
 * @example
 *   let add(a int, b int) int = { return a + b }  ✓ Valid
 *   @extern("printf") const printf(fmt *uint8)    ✓ Valid (no body)
 *   let foo() int = {}                            ✗ Error: missing return
 */
void checkFuncDecl(FuncDeclAST* func, SemanticContext& ctx);

// ============================================================================
// Struct Declaration
// ============================================================================

/**
 * @brief Validates a struct declaration.
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - No duplicate field names
 *   - Field default values match field type
 *   - Warns when fields with defaults appear before fields without defaults
 * 
 * @param structDecl The struct declaration AST node
 * @param ctx Semantic context for diagnostics
 * 
 * @example
 *   struct Vec2 { x float, y float }              ✓ Valid
 *   struct Point { x int, x int }                 ✗ Error: duplicate field 'x'
 *   struct Rect { x int = 0, y int }              ⚠ Warning: default before non-default
 */
void checkStructDecl(StructDeclAST* structDecl, SemanticContext& ctx);

// ============================================================================
// Enum Declaration
// ============================================================================

/**
 * @brief Validates an enum declaration.
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - No duplicate variant names
 *   - No duplicate explicit values
 *   - Auto-assigns values to variants without explicit values
 * 
 * @param enumDecl The enum declaration AST node
 * @param ctx Semantic context for diagnostics
 * 
 * @example
 *   enum Color { Red, Green, Blue }               ✓ Valid (Red=0, Green=1, Blue=2)
 *   enum Status { Ok=200, NotFound=404 }          ✓ Valid
 *   enum Flags { A=1, B=1 }                       ✗ Error: duplicate value 1
 */
void checkEnumDecl(EnumDeclAST* enumDecl, SemanticContext& ctx);

// ============================================================================
// Trait Declaration
// ============================================================================

/**
 * @brief Validates a trait declaration.
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - No duplicate method names
 *   - Method signatures are valid (no bodies allowed)
 * 
 * @param trait The trait declaration AST node
 * @param ctx Semantic context for diagnostics
 * 
 * @example
 *   trait Drawable { draw(), bounds() -> Rect }   ✓ Valid
 *   trait Bad { foo(), foo() }                    ✗ Error: duplicate method 'foo'
 */
void checkTraitDecl(TraitDeclAST* trait, SemanticContext& ctx);

// ============================================================================
// Impl Block
// ============================================================================

/**
 * @brief Validates an impl block (method implementations for a type).
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - No duplicate method names
 *   - If implementing a trait, all trait methods are implemented
 *   - Method signatures match trait (if applicable)
 *   - Method bodies are valid and return correct types
 * 
 * @param impl The impl declaration AST node
 * @param ctx Semantic context for diagnostics and scope
 * 
 * @example
 *   impl Vec2 { length() -> float = { return #sqrt(x*x + y*y) } }  ✓ Valid
 *   impl Drawable for Circle { draw() { ... } }                    ✓ Valid
 *   impl Trait for Type { missing() }                              ✗ Error: missing method
 */
void checkImplDecl(ImplDeclAST* impl, SemanticContext& ctx);

// ============================================================================
// From Block (Implicit Conversions)
// ============================================================================

/**
 * @brief Validates a from block (implicit conversion definitions).
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - Conversion signatures are valid
 *   - No duplicate source types for the same target
 *   - Inline entry bodies return the correct target type
 *   - Path entries reference callable functions
 * 
 * @param from The from declaration AST node
 * @param ctx Semantic context for diagnostics
 * 
 * @example
 *   from int {
 *       (s string) -> int = { return #parseInt(s) }
 *   }                                              ✓ Valid
 *   from string {
 *       (s string) -> int = { ... }                ✗ Error: source type 'string' is target??
 *   }
 */
void checkFromDecl(FromDeclAST* from, SemanticContext& ctx);

// ============================================================================
// Type Alias
// ============================================================================

/**
 * @brief Validates a type alias declaration.
 * 
 * Performs the following checks:
 *   - Validates all attributes on the declaration
 *   - Aliased type is valid (resolver already checked this)
 *   - No cyclic aliases (handled by TypeResolver)
 * 
 * @param alias The type alias declaration AST node
 * @param ctx Semantic context for diagnostics
 * 
 * @example
 *   type ID = int                                 ✓ Valid
 *   type Vec2 = struct { x float, y float }       ✓ Valid
 *   type A = B, type B = A                        ✗ Error: cyclic alias (in resolver)
 */
void checkTypeAliasDecl(TypeAliasDeclAST* alias, SemanticContext& ctx);

// ============================================================================
// Internal Helper Functions (Declaration-Specific Utilities)
// ============================================================================

/**
 * @namespace decl
 * @brief Internal utilities for declaration checking (not part of public API).
 * 
 * These helpers are used by the individual declaration checkers but are not
 * intended for external use. They are placed in a separate namespace to avoid
 * conflicts with other checker modules (ExprChecker, StmtChecker, etc.).
 */
namespace decl {

/**
 * @brief Validates all attributes on a declaration using the attribute registry.
 * 
 * This is an internal helper called by each declaration-specific checker.
 * It handles:
 *   - Unknown attribute detection
 *   - Context validation (can this attribute appear here?)
 *   - Argument count and type validation
 *   - Custom validation rules (e.g., @extern calling convention)
 *   - Mutual exclusion between conflicting attributes
 * 
 * @param decl The declaration with attributes to validate
 * @param ctx Semantic context for diagnostics
 * 
 * @note This function reports errors directly via SemanticContext::error()
 */
void validateAttributes(DeclAST* decl, SemanticContext& ctx);

/**
 * @brief Gets the human-readable name of a declaration for diagnostics.
 * 
 * Handles all declaration types appropriately:
 *   - ValueDeclAST: returns the declared name
 *   - TypeDeclAST: returns the type name
 *   - PackageDeclAST: returns the package name
 *   - Other types: returns appropriate placeholder strings
 * 
 * @param decl The declaration
 * @param pool String pool for name lookup
 * @return std::string The declaration name or a descriptive placeholder
 */
std::string getDeclName(const DeclAST* decl, const StringPool& pool);

/**
 * @brief Gets the declaration keyword (let/const) from a declaration.
 * 
 * @param decl The declaration (must be VarDeclAST or FuncDeclAST)
 * @return DeclKeyword The keyword, or DeclKeyword::Let as default for non-value decls
 */
DeclKeyword getDeclKeyword(const DeclAST* decl);

/**
 * @brief Converts a declaration kind to the appropriate AttributeContext bitmask.
 * 
 * Maps AST declaration kinds to the AttributeContext enum used by the
 * attribute registry for context validation.
 * 
 * @param decl The declaration AST node
 * @return AttributeContext Bitmask of valid contexts for this declaration type
 */
AttributeContext getAttributeContextForDecl(const DeclAST* decl);

/**
 * @brief Checks if a declaration has a specific attribute.
 * 
 * @param decl The declaration to check
 * @param attrId The interned ID of the attribute (e.g., attribute::getExternId())
 * @return true if the attribute is present
 */
bool hasAttribute(const DeclAST* decl, InternedString attrId);

/**
 * @brief Checks if a declaration has a specific attribute by name.
 * 
 * @param decl The declaration to check
 * @param attrName The attribute name string (e.g., "extern")
 * @param pool String pool for interning the name
 * @return true if the attribute is present
 */
bool hasAttribute(const DeclAST* decl, std::string_view attrName, const StringPool& pool);

} // namespace decl
