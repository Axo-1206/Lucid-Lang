#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Await Expression
// ============================================================================
// 
// parseAwaitExpr() parses the 'await' keyword used to suspend an async function.
// 
// Grammar: 'await' expr
// 
// Example: await httpGet(url)
// 
// ─── Semantic Restrictions (Parser Checks) ─────────────────────────────────
//   - 'await' is only valid inside a ~async function body
//   - 'await' is not allowed inside a ~parallel body (parallelDepth_ > 0)
//   - The awaited expression must be a call to a ~async function
// 
// The second restriction is checked here (parallelDepth_). The others are
// enforced by the semantic pass.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at 'await' keyword
// On exit:  positioned after the awaited expression
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
// None – single expression, no loops.
// ============================================================================

ExprPtr Parser::parseAwaitExpr(bool allowStructLiteral) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AWAIT, "expected 'await'");

    ExprPtr inner = parsePrattExpr(PREC_NONE, allowStructLiteral);
    if (!inner) {
        errorAt(DiagCode::E1008, "expected expression after 'await'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<AwaitExprAST>(std::move(inner));
    node->loc = loc;
    return node;
}