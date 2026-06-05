/**
 * @file StructResolver.cpp
 * @brief Implementation of struct field resolution.
 */

#include "StructResolver.hpp"
#include "../core/GenericParamHandler.hpp"
#include "debug/DebugMacros.hpp"

StructResolver::StructResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
    : ctx_(ctx), paramHandler_(paramHandler) {
    LUC_LOG_SEMANTIC("StructResolver constructed");
}

void StructResolver::resolve(StructDeclAST& node) {
    LUC_LOG_SEMANTIC("StructResolver::resolve: " << ctx_.pool.lookup(node.name));

    // Push generic parameters for field type resolution (resolved by caller).
    paramHandler_.pushParams(&node.genericParams);
    paramHandler_.popParams();

    // Create self-type for this struct if not already present.
    if (!node.selfType) {
        node.selfType = ctx_.arena.make<NamedTypeAST>(node.name);
        node.selfType->loc = node.loc;
        node.selfType->isGenericParam = false;
    }

    // Store resolved self-type in symbol table.
    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (sym && sym->kind == SymbolKind::Struct) {
        sym->type = node.selfType.get();
    }
}