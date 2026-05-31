#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Generic Arguments
// ============================================================================
// 
// parseGenericArgs() parses a generic argument list for type instantiation.
// 
// Grammar: '<' type { ',' type } '>'
// 
// Examples:
//   <int>
//   <string, Vec2>
//   <T, U, V>
//   <>                    (empty – allowed)
// 
// ─── Preconditions ────────────────────────────────────────────────────────
//   - The caller (parseNamedType or parsePostfixExpr) has already consumed
//     the opening '<' token
//   - This function starts immediately after the '<'
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
//   - If next token is '>', consumes it and returns empty span
//   - Otherwise parses comma‑separated types until '>'
//   - Consumes the closing '>'
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Uses saved position pattern with parseType()
//   - If parseType() makes no progress, consumes token and breaks
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '>' at end: consume() reports error
//   - Missing type after comma: reports error, breaks loop
//   - Empty list '<' '>' is valid (returns empty span)
// 
// ─── Return Value ─────────────────────────────────────────────────────────
//   Returns ArenaSpan<TypePtr> (temporary, caller converts to span)
// ============================================================================

ArenaSpan<TypePtr> Parser::parseGenericArgs() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<TypePtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        
        size_t savedPos = ts_.getPos();
        TypePtr arg = parseType();
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2005, "expected type in generic argument list");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        args.push_back(std::move(arg));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' to close generic arguments");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}