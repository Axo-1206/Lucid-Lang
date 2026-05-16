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
#include "ast/DeclAST.hpp"
#include "SymbolTable.hpp"
#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"

#include <unordered_map>
#include <string>

class DiagnosticEngine;

class TypeResolver : public ASTVisitor {
public:
    explicit TypeResolver(SymbolTable& symbols, DiagnosticEngine& dc, StringPool& pool, ASTArena& arena);

    TypeAST* resolveType(TypeAST* typeNode);

    // ── Type Nodes Visitor Overrides ──
    void visit(PrimitiveTypeAST& node)      override;
    void visit(NamedTypeAST& node)          override;
    void visit(NullableTypeAST& node)       override;
    void visit(FixedArrayTypeAST& node)     override;
    void visit(SliceTypeAST& node)          override;
    void visit(DynamicArrayTypeAST& node)   override;
    void visit(RefTypeAST& node)            override;
    void visit(PtrTypeAST& node)            override;
    void visit(FuncTypeAST& node)           override;

    // ── Declaration nodes Visitor Overrides ──
    void visit(FuncDeclAST& node)           override;
    void visit(VarDeclAST& node)            override;
    void visit(StructDeclAST& node)         override;
    void visit(ImplDeclAST& node)           override;
    void visit(MethodDeclAST& node)         override;
    void visit(FromDeclAST& node)           override;
    void visit(TypeAliasDeclAST& node)      override;
    void visit(TraitDeclAST& node)          override;
    void visit(TraitMethodAST& node)        override;
    void visit(TraitRefAST& node)           override;

    // ── Helper methods for resolving composite structures ──
    void resolveStructFields(StructDeclAST& node);
    void resolveFunctionType(FuncTypeAST& type);
    std::vector<TypeAST*> getFunctionReturnTypes(FuncTypeAST& type);
    TypeAST* getFunctionReturnType(FuncTypeAST& typeconst, const SourceLocation* loc = nullptr);

    // Call this before resolving types in an @extern-declared declaration
    // so that *T raw pointer types are permitted in that context.
    void setInsideExtern(bool val) { insideExtern_ = val; }

private:
    SymbolTable& symbols_;
    DiagnosticEngine& dc_;
    StringPool& pool_;
    ASTArena& arena_;
    TypeAST* resolved_ = nullptr;
    bool insideExtern_ = false;

    // Stack for nested generic parameters
    std::vector<const std::vector<GenericParamPtr>*> genericParamsStack_;
    
    // Stack for nested substitution maps (concrete type arguments)
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substitutionMapStack_;

    void pushGenericParams(const std::vector<GenericParamPtr>* params);
    void popGenericParams();
    bool isGenericParam(InternedString name) const;

    void pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map);
    void popSubstitutionMap();
    TypeAST* lookupSubstitution(InternedString name) const;
};