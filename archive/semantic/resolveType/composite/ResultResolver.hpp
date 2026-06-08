/**
 * @file ResultResolver.hpp
 * @brief Resolves result types (T!E and T!) - validates nesting rule.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

class ResultResolver {
public:
    explicit ResultResolver(SemanticContext& ctx) : ctx_(ctx) {}

    TypeAST* resolve(ResultTypeAST& node) {
        // Grammar: No nested '!'
        if (node.inner && node.inner->isa<ResultTypeAST>()) {
            ctx_.error(node.loc, DiagCode::E1021,
                       "result type cannot nest '!' (use alias)");
            return nullptr;
        }
        if (node.errorType && node.errorType->isa<ResultTypeAST>()) {
            ctx_.error(node.loc, DiagCode::E1021,
                       "error type cannot carry '!' (use alias)");
            return nullptr;
        }
        return &node;
    }

private:
    SemanticContext& ctx_;
};