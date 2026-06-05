/**
 * @file TypeAliasResolver.hpp
 * @brief Resolves type aliases - pushes generic params, resolves RHS.
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "../core/GenericParamHandler.hpp"
#include "debug/DebugMacros.hpp"

class TypeAliasResolver {
public:
    TypeAliasResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
        : ctx_(ctx), paramHandler_(paramHandler) {}

    void resolve(TypeAliasDeclAST& node) {
        LUC_LOG_SEMANTIC("TypeAliasResolver::resolve: " << ctx_.pool.lookup(node.name));

        // Push generic parameters for resolution of the RHS
        paramHandler_.pushParams(&node.genericParams);

        // The aliased type will be resolved by the caller (TypeDispatcher)
        // We don't resolve it here to avoid double resolution.

        paramHandler_.popParams();

        // Store resolved type in symbol table (done by caller)
    }

private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};