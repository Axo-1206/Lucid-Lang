#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Reference Type
// ============================================================================
// 
// parseRefType() parses a safe managed reference type.
// 
// Grammar: '&' type
// 
// Example: `&int`, `&Vec2`
// 
// ─── Semantics ────────────────────────────────────────────────────────────
//   - References are always valid (non‑nullable by default)
//   - To express a nullable reference: `&T?` where '?' attaches to T
//   - Used for shared ownership without copying
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '&'
// On exit:  positioned after the inner type
// 
// ─── Note ─────────────────────────────────────────────────────────────────
//   - The inner type is parsed via parseBaseType() (not parseType()) to avoid
//     consuming a trailing '?' that belongs to the inner type
//   - RefTypeAST itself is not wrapped in nullable – '?' lives on inner type
// ============================================================================

TypePtr Parser::parseRefType() {
    LUC_LOG_TYPE_EXTREME("parseRefType: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AMPERSAND, "expected '&'");
    
    LUC_LOG_TYPE_EXTREME("parseRefType: parsing inner type");
    TypePtr inner = parseBaseType();
    if (!inner) {
        LUC_LOG_TYPE("parseRefType: ERROR - expected type after '&'");
        // Use E1005: "Expected type annotation" (parsing error)
        errorAt(DiagCode::E1005, "expected type after '&'");
        return arena_.make<UnknownTypeAST>();
    }
    
    auto node = arena_.make<RefTypeAST>(inner);
    node->loc = loc;
    LUC_LOG_TYPE_EXTREME("parseRefType: success");
    return node;
}