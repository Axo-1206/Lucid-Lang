/**
 * @file ArrayResolver.hpp
 * @brief Resolves array types - validates fixed size.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

class ArrayResolver {
public:
    explicit ArrayResolver(SemanticContext& ctx) : ctx_(ctx) {}

    TypeAST* resolve(ArrayTypeAST& node) {
        if (node.arrayKind == ArrayKind::Fixed && node.size == 0) {
            ctx_.error(node.loc, DiagCode::E2002,
                       "fixed array size must be greater than zero");
        }
        return &node;
    }

private:
    SemanticContext& ctx_;
};