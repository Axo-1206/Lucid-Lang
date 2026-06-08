/**
 * @file PrimitiveResolver.hpp
 * @brief Resolves primitive types - trivial.
 */

#pragma once

#include "ast/TypeAST.hpp"

struct SemanticContext;

class PrimitiveResolver {
public:
    explicit PrimitiveResolver(SemanticContext& ctx) : ctx_(ctx) {}

    TypeAST* resolve(PrimitiveTypeAST& node) {
        // Primitive types are self-contained; no further resolution needed.
        return &node;
    }

private:
    SemanticContext& ctx_;
};