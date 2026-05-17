/**
 * @file TypeChecker.hpp
 * @responsibility Provides type compatibility checks between resolved types.
 *
 * TypeChecker is a utility class (converted to instance-based with
 * StringPool and ASTArena) that answers questions like:
 *   - Can type A be assigned to type B?
 *   - Are two types structurally equal?
 *   - Is a type callable (function type)?
 *   - What is the unified type of two branches?
 *   - Is a type integer, boolean, nullable, or reference‑comparable?
 *
 * It is used by Phase 2b (type resolution validation) and Phase 3 (full checking)
 * to enforce Luc's type system rules without walking the AST itself.
 *
 * @related
 *   - TypeResolver.hpp – resolves type names to nodes
 *   - SemanticExpr.cpp, SemanticStmt.cpp – call type checking helpers
 *   - TypeAST.hpp – the types being compared
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Why a separate type checker is necessary
 * ─────────────────────────────────────────────────────────────────────────────
 * - Type compatibility logic is complex (widening, nullability, generics,
 *   function signatures, custom `from` conversions). Factoring it into a
 *   dedicated class keeps the semantic passes focused.
 * - Many semantic rules (assignment, return type matching, binary operator
 *   validity) reuse the same underlying compatibility predicates.
 * - Centralising the logic avoids duplication and inconsistency.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Design decisions
 * ─────────────────────────────────────────────────────────────────────────────
 * - **Instance‑based (non‑static)** – the checker holds references to
 *   StringPool and ASTArena, making dependencies explicit and avoiding
 *   hidden global state. All methods are non‑static.
 * - **No AST traversal** – the checker operates solely on TypeAST nodes.
 *   It does not walk the AST; callers are responsible for providing the
 *   types to compare.
 * - **StringPool for name display** – diagnostic messages use the pool to
 *   convert InternedString type names to readable strings.
 * - **Arena for temporary types** – the arena is used when the checker
 *   needs to synthesise types (e.g., during unification or slice type
 *   creation), though most checks are read‑only.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * Usage example
 * ─────────────────────────────────────────────────────────────────────────────
 * @code
 * TypeChecker checker(pool, arena);
 *
 * // During assignment checking
 * if (!checker.isAssignable(fromType, toType)) {
 *     dc.error(...);
 * }
 *
 * // During binary operator validation
 * if (!checker.isIntegerType(leftType) || !checker.isIntegerType(rightType)) {
 *     dc.error("bitwise operators require integer operands");
 * }
 *
 * // During match expression unification
 * TypeAST* unified = checker.unify(arm1Type, arm2Type);
 * @endcode
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note All methods assume that the input TypeAST nodes have already been
 *       resolved by TypeResolver. Passing unresolved types (e.g., a NamedTypeAST
 *       that still points to an unknown symbol) leads to undefined behaviour.
 * ─────────────────────────────────────────────────────────────────────────────
 */
#pragma once

#include "SymbolTable.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "ast/TypeAST.hpp"
#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"
#include <cassert>
#include <string>

class TypeChecker {
public:
    explicit TypeChecker(StringPool& pool, ASTArena& arena);

    // ── Type compatibility ───────────────────────────────────────────────────
    bool isEqual(TypeAST* a, TypeAST* b);
    bool isAssignable(TypeAST* from, TypeAST* to);
    bool areAssignableMultiple(const std::vector<TypeAST*>& fromTypes,
                                const std::vector<TypeAST*>& toTypes);
    bool isCallable(TypeAST* type);
    bool isBooleanCompatible(TypeAST* type);
    bool isNullable(TypeAST* type);
    TypeAST* unify(TypeAST* a, TypeAST* b);
    bool primitiveWidening(PrimitiveKind from, PrimitiveKind to);
    
    // Requires StringPool to convert InternedString to string for prefix search
    Symbol* isFromCastable(TypeAST* src, TypeAST* target, SymbolTable* symbols);

    // ── Integer type validation ───────────────────────────────────────────────
    bool isIntegerType(TypeAST* type);
    
    bool isValidArrayIndex(ExprAST* indexExpr, DiagnosticEngine& dc, 
                           const SourceLocation& loc);
    
    bool isValidSliceBound(ExprAST* boundExpr, const std::string& boundName,
                           DiagnosticEngine& dc, const SourceLocation& loc);
    
    bool getConstantIntValue(ExprAST* expr, int64_t& outValue);

    // ── Comparison validity helpers ───────────────────────────────────────────
    bool isValueComparable(TypeAST* type, SymbolTable* symbols = nullptr);
    bool isReferenceComparable(TypeAST* type);
    bool isBoolOrNullable(TypeAST* type);

private:
    StringPool& pool_;
    ASTArena& arena_;
};