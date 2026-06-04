#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* TypeResolver::resolveResultType(ResultTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveResultType");
    
    // Reject nested '!' – grammar rule: neither inner nor errorType may be ResultTypeAST
    if (node.inner && node.inner->isa<ResultTypeAST>()) {
        ctx_.error(node.loc, DiagCode::E2002, "result type cannot nest '!'");
        return nullptr;
    }
    if (node.errorType && node.errorType->isa<ResultTypeAST>()) {
        ctx_.error(node.loc, DiagCode::E2002, "error type cannot carry '!'");
        return nullptr;
    }
    
    if (node.inner) resolveType(node.inner.get());
    if (node.errorType) resolveType(node.errorType.get());
    return &node;
}