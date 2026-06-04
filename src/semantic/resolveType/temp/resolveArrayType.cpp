#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* TypeResolver::resolveArrayType(ArrayTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolveArrayType: kind=" << static_cast<int>(node.arrayKind));
    if (node.arrayKind == ArrayKind::Fixed && node.size == 0) {
        ctx_.error(node.loc, DiagCode::E2002, "fixed array size must be greater than zero");
    }
    if (node.element) resolveType(node.element.get());
    return &node;
}