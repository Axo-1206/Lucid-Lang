#include "TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* TypeResolver::resolvePrimitiveType(PrimitiveTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("resolvePrimitiveType: kind=" << static_cast<int>(node.primitiveKind));
    return &node;
}