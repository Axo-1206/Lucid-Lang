/**
 * @file NamedResolver.hpp
 * @brief Resolves named types (user-defined types, generic parameters).
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include <vector>

class GenericParamHandler;

class NamedResolver {
public:
    NamedResolver(SemanticContext& ctx, GenericParamHandler& paramHandler);

    TypeAST* resolve(NamedTypeAST& node);

private:
    TypeAST* resolveGenericParam(NamedTypeAST& node);
    TypeAST* resolveConcreteType(NamedTypeAST& node);
    TypeAST* unwrapTypeAlias(NamedTypeAST& node, Symbol* sym);
    void validateGenericConstraints(NamedTypeAST& node, Symbol* structSym);

    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};