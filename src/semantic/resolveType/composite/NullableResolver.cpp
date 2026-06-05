/**
 * @file NullableResolver.cpp
 * @brief Implementation of nullable type resolution.
 */

#include "NullableResolver.hpp"
#include "FuncResolver.hpp"
#include "../core/ConstraintChecker.hpp"
#include "debug/DebugMacros.hpp"

NullableResolver::NullableResolver(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC("NullableResolver constructed");
}

TypeAST* NullableResolver::resolve(NullableTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("NullableResolver::resolve");

    if (!node.inner) {
        ctx_.error(node.loc, DiagCode::E2002, "nullable type missing inner type");
        return nullptr;
    }

    // Grammar: '?' attaches to value types only.
    if (node.inner->isa<FuncTypeAST>()) {
        ctx_.error(node.loc, DiagCode::E1019,
                   "'?' cannot be applied directly to a function type. "
                   "Use a type alias: type Fn = (int)->int; let f Fn? = nil");
        return nullptr;
    }

    // Double nullable not allowed.
    if (node.inner->isa<NullableTypeAST>()) {
        ctx_.error(node.loc, DiagCode::E2033, "double nullable (e.g., int\?\?) is not allowed");
        return nullptr;
    }

    // Inner type will be resolved by caller.
    return &node;
}