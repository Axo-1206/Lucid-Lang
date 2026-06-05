/**
 * @file StructResolver.hpp
 * @brief Resolves struct field types and creates self-type.
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class GenericParamHandler;

class StructResolver {
public:
    StructResolver(SemanticContext& ctx, GenericParamHandler& paramHandler);

    void resolve(StructDeclAST& node);

private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};