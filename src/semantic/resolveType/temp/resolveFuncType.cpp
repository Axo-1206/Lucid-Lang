#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* TypeResolver::resolveFuncType(FuncTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveFuncType");

    // Resolve qualifiers
    for (const auto& qualName : node.rawQualifiers) {
        const QualifierEntry* entry = qualifier::lookup(qualName);
        if (!entry) {
            ctx_.error(node.loc, DiagCode::E1016, "~", ctx_.pool.lookup(qualName));
        } else {
            node.qualifiers |= entry->bit;
        }
    }

    // Resolve parameters
    for (auto& param : node.sig.allParams) {
        if (param && param->type) resolveType(param->type.get());
    }

    // Resolve return types
    for (auto& retType : node.sig.returnTypes) {
        if (retType) resolveType(retType.get());
    }

    return &node;
}