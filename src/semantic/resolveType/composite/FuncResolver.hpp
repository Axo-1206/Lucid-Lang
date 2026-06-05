/**
 * @file FuncResolver.hpp
 * @brief Resolves function types with qualifiers, parameters, and return types.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include <vector>

class GenericParamHandler;

class FuncResolver {
public:
    FuncResolver(SemanticContext& ctx, GenericParamHandler& paramHandler);

    TypeAST* resolve(FuncTypeAST& node);
    TypeAST* getReturnType(const FuncTypeAST& type, const SourceLocation* loc);
    std::vector<TypeAST*> getReturnTypes(const FuncTypeAST& type);

private:
    void resolveQualifiers(FuncTypeAST& node);
    void resolveParameters(FuncTypeAST& node);
    void resolveReturnTypes(FuncTypeAST& node);

    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};