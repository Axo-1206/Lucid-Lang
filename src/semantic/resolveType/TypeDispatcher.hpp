/**
 * @file TypeDispatcher.hpp
 * @brief Main entry point for type resolution - dispatches to specialized resolvers.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "core/GenericParamHandler.hpp"
#include <memory>

// Forward declarations
class PrimitiveResolver;
class NamedResolver;
class NullableResolver;
class ResultResolver;
class ArrayResolver;
class RefResolver;
class PtrResolver;
class FuncResolver;
class TypeAliasResolver;
class StructResolver;
class FuncSignatureResolver;
class InjectionTransformer;
class CallableExtractor;
class ImplResolver;
class FromResolver;
class VarResolver;

class TypeDispatcher {
public:
    explicit TypeDispatcher(SemanticContext& ctx);
    ~TypeDispatcher();

    TypeAST* resolveType(TypeAST* typeNode);

    void resolveTypeAlias(TypeAliasDeclAST& node);
    void resolveStructFields(StructDeclAST& node);
    void resolveFunctionSignature(FuncDeclAST& node);
    void resolveImplMethods(ImplDeclAST& node);
    void resolveFromEntries(FromDeclAST& node);
    void resolveVarType(VarDeclAST& node);

    GenericParamHandler& genericParams() { return genericParams_; }

    TypeAST* getFunctionReturnType(const FuncTypeAST& type, const SourceLocation* loc);
    std::vector<TypeAST*> getFunctionReturnTypes(const FuncTypeAST& type);

private:
    SemanticContext& ctx_;

    // Stateful core
    GenericParamHandler genericParams_;

    // Simple resolvers (no cross-dependencies)
    std::unique_ptr<PrimitiveResolver> primitiveResolver_;
    std::unique_ptr<NamedResolver> namedResolver_;
    std::unique_ptr<ResultResolver> resultResolver_;
    std::unique_ptr<ArrayResolver> arrayResolver_;
    std::unique_ptr<RefResolver> refResolver_;
    std::unique_ptr<PtrResolver> ptrResolver_;
    std::unique_ptr<FuncResolver> funcResolver_;
    std::unique_ptr<TypeAliasResolver> typeAliasResolver_;
    std::unique_ptr<StructResolver> structResolver_;
    std::unique_ptr<FuncSignatureResolver> funcSignatureResolver_;
    std::unique_ptr<VarResolver> varResolver_;

    // Dependencies for complex resolvers (must be declared before them)
    std::unique_ptr<NullableResolver> nullableResolver_;   // depends on FuncResolver
    std::unique_ptr<InjectionTransformer> injectionTransformer_;
    std::unique_ptr<CallableExtractor> callableExtractor_;

    // Complex resolvers (depend on above)
    std::unique_ptr<ImplResolver> implResolver_;
    std::unique_ptr<FromResolver> fromResolver_;
};