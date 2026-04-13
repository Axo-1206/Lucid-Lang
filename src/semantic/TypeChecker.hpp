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
    static bool isFromConvertible(TypeAST* src, TypeAST* target);
};