/**
 * @file RefResolver.hpp
 * @brief Resolves reference types (&T) - trivial.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class RefResolver {
public:
    explicit RefResolver(SemanticContext& ctx) : ctx_(ctx) {}

    TypeAST* resolve(RefTypeAST& node) {
        // References are simple; inner type resolved by caller.
        return &node;
    }

private:
    SemanticContext& ctx_;
};