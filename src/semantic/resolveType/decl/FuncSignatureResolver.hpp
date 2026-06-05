/**
 * @file FuncSignatureResolver.hpp
 * @brief Resolves function signatures - pushes generic params.
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "../core/GenericParamHandler.hpp"
#include "debug/DebugMacros.hpp"

class FuncSignatureResolver {
public:
    FuncSignatureResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
        : ctx_(ctx), paramHandler_(paramHandler) {}

    void resolve(FuncDeclAST& node) {
        LUC_LOG_SEMANTIC("FuncSignatureResolver::resolve: " << ctx_.pool.lookup(node.name));

        // Push generic parameters for resolution of the function type
        paramHandler_.pushParams(&node.genericParams);

        // The function type will be resolved by the caller (TypeDispatcher)

        paramHandler_.popParams();

        // Store resolved type in symbol table (done by caller)
    }

private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};