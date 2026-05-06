/**
 * @file TypeChecker.hpp
 *
 * @nutshell Interrogates pairs of types for compatiblity.
 *
 * @reason Declares the boundary utility functions cleanly so multiple distinct phases (Decl, Stmt, Expr) can reuse boolean unification checks.
 *
 * @responsibility Phase 2b of semantic analysis: provides compatibility checks between already-resolved types.
 *
 * @logic Pure utility class; provides unification and logic for evaluating convertibility and assignability, without walking the AST itself.
 *
 * @related TypeChecker.cpp, SemanticExpr.cpp, SemanticStmt.cpp
 */
#pragma once

#include "SymbolTable.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "ast/TypeAST.hpp"
#include <cassert>

class TypeChecker {
public:
    static bool isEqual(TypeAST* a, TypeAST* b);
    static bool isAssignable(TypeAST* from, TypeAST* to);
    static bool isCallable(TypeAST* type);
    static bool isBooleanCompatible(TypeAST* type);
    static bool isNullable(TypeAST* type);
    static TypeAST* unify(TypeAST* a, TypeAST* b);
    static bool primitiveWidening(PrimitiveKind from, PrimitiveKind to);
    static bool isFromCastable(TypeAST* src, TypeAST* target, SymbolTable* symbols = nullptr);

    // ── Integer type validation ───────────────────────────────────────────────
    
    // isIntegerType — returns true when a type is any integer primitive.
    // Used for: shift operations, bitwise ops, array index type validation.
    static bool isIntegerType(TypeAST* type);
    
    // isValidArrayIndex — checks BOTH type and value constraints for array indexing.
    // Returns: true if the index can be used (with runtime bounds check if needed)
    //          false with error message if invalid at compile time.
    // 
    // For constant expressions (literals): validates value >= 0 at compile time.
    // For variable expressions: validates type is integer (runtime panic handles negative).
    static bool isValidArrayIndex(ExprAST* indexExpr, DiagnosticEngine& dc, 
                                   const SourceLocation& loc);
    
    // isValidSliceBound — checks BOTH type and value constraints for slice bounds.
    // For slice expressions like arr[start..end] where both bounds are constant.
    // Enforces: start >= 0, end >= start, both are constants.
    static bool isValidSliceBound(ExprAST* boundExpr, const std::string& boundName,
                                   DiagnosticEngine& dc, const SourceLocation& loc);
    
    // Get constant integer value from expression (if compile-time known)
    // Returns true and sets value if expression is a constant integer literal.
    static bool getConstantIntValue(ExprAST* expr, int64_t& outValue);

    // ── Comparison validity helpers ───────────────────────────────────────────

    // isValueComparable — returns true when == and != are valid on this type.
    //
    // Valid for:
    //   primitives (int, float, bool, char, string, enum variants)
    //   nullable types (int?, Vec2? — compares value or nil)
    //
    // NOT valid for (semantic error — emit E3011/E3012/E3013):
    //   struct types  → developer must implement Equatable<T> and use :equals()
    //   function types → function bodies are incomparable
    //   array types   → use collection library comparison function
    static bool isValueComparable(TypeAST* type, SymbolTable* symbols = nullptr);

    // isReferenceComparable — returns true when === is valid on this type.
    //
    // Valid for:
    //   &T reference types
    //   struct types (compares memory addresses, not field values)
    //   nullable types containing the above
    //
    // NOT valid for:
    //   primitives — primitives are value types with no stable address
    //   function types
    //   array types
    static bool isReferenceComparable(TypeAST* type);

    // isBoolOrNullable — returns true when a type is valid for 'not', 'and', 'or'.
    //
    // Valid for:
    //   bool
    //   any nullable type T? (nil treated as false, non-nil as true)
    //
    // NOT valid for:
    //   int, float, string, struct, function, array
    static bool isBoolOrNullable(TypeAST* type);
};