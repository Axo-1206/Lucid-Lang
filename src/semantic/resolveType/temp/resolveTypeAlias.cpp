#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

void TypeResolver::resolveTypeAlias(TypeAliasDeclAST& node) {
    LUC_LOG_SEMANTIC("resolveTypeAlias: " << ctx_.pool.lookup(node.name));
    pushGenericParams(&node.genericParams);
    TypeAST* resolved = node.aliasedType ? resolveType(node.aliasedType.get()) : nullptr;
    popGenericParams();

    Symbol* sym = ctx_.symbols->lookup(node.name);
    if (sym && resolved) sym->type = resolved;
}