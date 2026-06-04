#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* TypeResolver::resolveNullableType(NullableTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveNullableType");
    
    // Grammar rule: '?' attaches to value types only
    // Check that inner type is a value type (not a reference, not a function type)
    if (node.inner) {
        TypeAST* resolvedInner = resolveType(node.inner.get());
        if (resolvedInner && resolvedInner->isa<FuncTypeAST>()) {
            ctx_.error(node.loc, DiagCode::E2002, 
                      "'?' cannot be applied directly to a function type. Use a type alias: type Fn = (int)->int; let f Fn? = nil");
            return nullptr;
        }
    }
    
    return &node;
}