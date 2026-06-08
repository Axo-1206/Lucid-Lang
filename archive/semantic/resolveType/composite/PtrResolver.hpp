/**
 * @file PtrResolver.hpp
 * @brief Resolves raw pointer types (*T) - trivial.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class PtrResolver {
public:
    explicit PtrResolver(SemanticContext& ctx) : ctx_(ctx) {}

    TypeAST* resolve(PtrTypeAST& node) {
        // Pointers are simple; inner type resolved by caller.
        return &node;
    }

private:
    SemanticContext& ctx_;
};