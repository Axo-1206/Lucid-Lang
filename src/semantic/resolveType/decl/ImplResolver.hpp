/**
 * @file ImplResolver.hpp
 * @brief Resolves impl declarations - most complex resolver.
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "../core/GenericParamHandler.hpp"
#include "../injection/InjectionTransformer.hpp"
#include "../callable/CallableExtractor.hpp"

class ImplResolver {
public:
    ImplResolver(SemanticContext& ctx,
                 GenericParamHandler& paramHandler,
                 InjectionTransformer& injector,
                 CallableExtractor& callableExtractor);

    void resolve(ImplDeclAST& node);

private:
    TypeAST* resolveTargetType(ImplDeclAST& node);
    TypeAST* unwrapTargetType(TypeAST* target);
    void validateGenericArity(ImplDeclAST& node, TypeAST* underlying,
                              const ArenaSpan<GenericParamPtr>* targetGenericParams);
    void buildSubstitutionMap(ImplDeclAST& node, TypeAST* target,
                              const ArenaSpan<GenericParamPtr>* targetGenericParams);
    void resolveMethods(ImplDeclAST& node, const std::string& typeName);
    void resolveMethodInline(MethodDeclAST& method);
    void resolveMethodPlainAssignment(MethodDeclAST& method);
    void resolveMethodInjectionAssignment(MethodDeclAST& method);

    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    InjectionTransformer& injector_;
    CallableExtractor& callableExtractor_;
};