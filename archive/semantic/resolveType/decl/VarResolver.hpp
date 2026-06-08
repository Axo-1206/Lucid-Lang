/**
 * @file VarResolver.hpp
 * @brief Resolves variable types - simple.
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"

class VarResolver {
public:
    explicit VarResolver(SemanticContext& ctx) : ctx_(ctx) {}

    void resolve(VarDeclAST& node) {
        LUC_LOG_SEMANTIC("VarResolver::resolve: " << ctx_.pool.lookup(node.name));

        // Variable type resolution is simple - store the type.
        // Actual type resolution is done by the caller (TypeDispatcher).
        Symbol* sym = ctx_.symbols->lookup(node.name);
        if (sym && sym->kind == SymbolKind::Var && node.type) {
            sym->type = node.type.get();
        }
    }

private:
    SemanticContext& ctx_;
};