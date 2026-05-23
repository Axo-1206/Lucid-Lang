/**
 * @file TypeResolver.hpp
 * ... (existing documentation) ...
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "SymbolTable.hpp"
#include "ast/support/StringPool.hpp"
#include "ast/support/ASTArena.hpp"
#include "ast/support/ArenaSpan.hpp"  // Add this include

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

    void setCurrentFile(InternedString file) { currentFile_ = file; }

    // ── Helper methods for resolving composite structures ──
    TypeAST* cloneType(const TypeAST* type);
    FuncTypeAST* cloneFuncSignature(const FuncSignature& sig, const SourceLocation& loc);
    void resolveStructFields(StructDeclAST& node);
    void resolveFunctionType(const FuncTypeAST& type);
    std::vector<TypeAST*> getFunctionReturnTypes(const FuncTypeAST& type);
    TypeAST* getFunctionReturnType(const FuncTypeAST& typeconst, const SourceLocation* loc = nullptr);

    // Call this before resolving types in an @extern-declared declaration
    void setInsideExtern(bool val) { insideExtern_ = val; }

    void setStructTraits(const std::unordered_map<InternedString, std::vector<InternedString>>* map) {
        structTraits_ = map;
    }

    StringPool& getPool() const { return pool_; }
    ASTArena& getArena() const { return arena_; }

    // Updated to use ArenaSpan
    void pushGenericParams(const ArenaSpan<GenericParamPtr>* params);
    void popGenericParams();
    bool isGenericParam(InternedString name) const;

    void pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map);
    void popSubstitutionMap();
    TypeAST* lookupSubstitution(InternedString name) const;

private:
    SymbolTable& symbols_;
    InternedString currentFile_;
    DiagnosticEngine& dc_;
    StringPool& pool_;
    ASTArena& arena_;
    TypeAST* resolved_ = nullptr;
    bool insideExtern_ = false;
    const std::unordered_map<InternedString, std::vector<InternedString>>* structTraits_ = nullptr;

    // Stack for nested generic parameters - now using ArenaSpan
    std::vector<const ArenaSpan<GenericParamPtr>*> genericParamsStack_;
    
    // Stack for nested substitution maps (concrete type arguments)
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substitutionMapStack_;
    
    void resolveGenericParamConstraints(GenericParamAST& gp);
    bool satisfiesConstraints(TypeAST* type, const std::vector<InternedString>& requiredTraits) const;
};