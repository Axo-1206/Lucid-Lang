/**
 * @file TypeResolver.hpp
 * @brief Resolves type names and annotations using switch‑case dispatch.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include <unordered_map>

struct SemanticContext;

class TypeResolver {
public:
    explicit TypeResolver(SemanticContext& ctx);

    // Main entry point
    TypeAST* resolveType(TypeAST* typeNode);

    // Declaration‑level resolution (for Phase 2)
    void resolveTypeAlias(TypeAliasDeclAST& node);
    void resolveStructFields(StructDeclAST& node);
    void resolveFunctionSignature(FuncDeclAST& node);
    void resolveImplMethods(ImplDeclAST& node);
    void resolveFromEntries(FromDeclAST& node);
    void resolveVarType(VarDeclAST& node);

    // Generic parameter stack management
    void pushGenericParams(const ArenaSpan<GenericParamPtr>* params);
    void popGenericParams();
    bool isGenericParam(InternedString name) const;

    // Substitution map (for generic instantiation)
    void pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map);
    void popSubstitutionMap();
    TypeAST* lookupSubstitution(InternedString name) const;

    // Constraint checking
    bool satisfiesConstraints(TypeAST* type, const std::vector<InternedString>& requiredTraits) const;

    // Cloning utilities
    TypeAST* cloneType(const TypeAST* type);
    FuncTypeAST* cloneFuncType(const FuncTypeAST* src, const SourceLocation& loc);

    // Trait mapping (set by SemanticAnalyzer)
    void setStructTraits(const std::unordered_map<InternedString, std::vector<InternedString>>* map) {
        structTraits_ = map;
    }

    // Helper to get function return type(s)
    TypeAST* getFunctionReturnType(const FuncTypeAST& type, const SourceLocation* loc);
    std::vector<TypeAST*> getFunctionReturnTypes(const FuncTypeAST& type);

private:
    SemanticContext& ctx_;  // Reference to the semantic context

    // Dispatch helpers
    TypeAST* resolvePrimitiveType(PrimitiveTypeAST& node);
    TypeAST* resolveNamedType(NamedTypeAST& node);
    TypeAST* resolveNullableType(NullableTypeAST& node);
    TypeAST* resolveResultType(ResultTypeAST& node);
    TypeAST* resolveArrayType(ArrayTypeAST& node);
    TypeAST* resolveRefType(RefTypeAST& node);
    TypeAST* resolvePtrType(PtrTypeAST& node);
    TypeAST* resolveFuncType(FuncTypeAST& node);

    // Internal helper for cloning function type (used by cloneFuncType)
    FuncTypeAST* cloneFuncTypeInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc);

    // Generic stacks
    std::vector<const ArenaSpan<GenericParamPtr>*> genericParamsStack_;
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substitutionMapStack_;
    const std::unordered_map<InternedString, std::vector<InternedString>>* structTraits_ = nullptr;
};