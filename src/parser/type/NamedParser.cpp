#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Named Type
// ============================================================================
// 
// parseNamedType() parses a user-defined type reference with optional generic
// arguments.
// 
// Grammar: IDENTIFIER [ '<' type { ',' type } '>' ]
// 
// Examples:
//   Vec2
//   Buffer<int>
//   Map<string, Vec2>
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at IDENTIFIER (type name)
// On exit:  positioned after generic arguments (or after name if none)
// 
// ─── Generic Arguments ────────────────────────────────────────────────────
//   - Parsed via parseGenericArgs() (consumes '<' ... '>')
//   - Empty list `<` `>` is allowed
//   - Semantic pass validates argument count against declaration
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing type name: returns UnknownTypeAST
// - Generic argument errors: reported by parseGenericArgs()
// ============================================================================

TypePtr Parser::parseNamedType() {
    SourceLocation loc = ts_.currentLoc();
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type name");
        return arena_.make<UnknownTypeAST>();
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    auto node = arena_.make<NamedTypeAST>(name);
    node->loc = loc;

    if (ts_.check(TokenType::LESS)) {
        node->genericArgs = parseGenericArgs();
    }

    return node;
}