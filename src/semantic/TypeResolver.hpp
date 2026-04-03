/**
 * @file TypeResolver.hpp
 *
 * @nutshell The pipeline that asserts a typed name actually officially exists.
 *
 * @reason Serves as an independent step explicitly dedicated to resolving user-claimed string types into confirmed engine structures prior to usage in functions.
 *
 * @responsibility Phase 2a of semantic analysis: validate that every parsed type name resolves to a valid symbol.
 *
 * @logic Takes a raw TypeAST* from the parser and returns the resolved TypeAST* or emits a diagnostic for invalid types.
 *
 * @related TypeResolver.cpp, SemanticAnalyzer.hpp
 */
#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/SymbolTable.hpp"

class DiagnosticEngine;

class TypeResolver : public ASTVisitor {
public:
    explicit TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc);

    TypeAST* resolveType(TypeAST* typeNode);

    // ── Type Nodes Visitor Overrides ──
    void visit(PrimitiveTypeAST& node)      override;
    void visit(NamedTypeAST& node)          override;
    void visit(NullableTypeAST& node)       override;
    void visit(UnionTypeAST& node)          override;
    void visit(FixedArrayTypeAST& node)     override;
    void visit(SliceTypeAST& node)          override;
    void visit(DynamicArrayTypeAST& node)   override;
    void visit(RefTypeAST& node)            override;
    void visit(PtrTypeAST& node)            override;
    void visit(FuncTypeAST& node)           override;

    // Call this before resolving types in an extern block so @T is permitted.
    void setInsideExtern(bool val) { insideExtern_ = val; }

private:
    SymbolTable& symbols_;
    DiagnosticEngine& dc_;
    TypeAST* resolved_ = nullptr;
    bool insideExtern_ = false;
};
