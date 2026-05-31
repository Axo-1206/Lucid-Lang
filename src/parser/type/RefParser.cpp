#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

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
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AMPERSAND, "expected '&'");
    TypePtr inner = parseBaseType();
    if (!inner) {
        errorAt(DiagCode::E2005, "expected type after '&'");
        return arena_.make<UnknownTypeAST>();
    }
    auto node = arena_.make<RefTypeAST>(std::move(inner));
    node->loc = loc;
    return node;
}