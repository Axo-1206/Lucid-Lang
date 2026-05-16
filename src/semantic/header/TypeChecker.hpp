/**
 * @file TypeChecker.hpp
 *
 * @nutshell Interrogates pairs of types for compatibility.
 *
 * @reason Declares the boundary utility functions cleanly so multiple distinct phases (Decl, Stmt, Expr) can reuse boolean unification checks.
 *
 * @responsibility Phase 2b of semantic analysis: provides compatibility checks between already-resolved types.
 *
 * @logic Provides unification and logic for evaluating convertibility and assignability, without walking the AST itself.
 *
 * @related TypeChecker.cpp, SemanticExpr.cpp, SemanticStmt.cpp
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
    bool isCallable(TypeAST* type);
    bool isBooleanCompatible(TypeAST* type);
    bool isNullable(TypeAST* type);
    TypeAST* unify(TypeAST* a, TypeAST* b);
    bool primitiveWidening(PrimitiveKind from, PrimitiveKind to);
    
    // Requires StringPool to convert InternedString to string for prefix search
    bool isFromCastable(TypeAST* src, TypeAST* target, SymbolTable* symbols);

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