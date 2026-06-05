/**
 * @file NullableResolver.hpp
 * @brief Resolves nullable types (T?) with validation.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class FuncResolver;

class NullableResolver {
public:
    explicit NullableResolver(SemanticContext& ctx);
    void setFuncResolver(FuncResolver* resolver) { funcResolver_ = resolver; }

    TypeAST* resolve(NullableTypeAST& node);

private:
    SemanticContext& ctx_;
    FuncResolver* funcResolver_ = nullptr;
};