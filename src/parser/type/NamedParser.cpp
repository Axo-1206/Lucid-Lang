#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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
    LUC_LOG_TYPE_VERBOSE("parseNamedType: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    // Log current token before checking
    LUC_LOG_TYPE("parseNamedType: current token = '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_TYPE("parseNamedType: ERROR - expected identifier at line " 
                     << ts_.peek().line << ", col " << ts_.peek().column);
        errorAt(DiagCode::E1003, "expected identifier for named type");
        return arena_.make<UnknownTypeAST>();
    }

    SourceLocation loc = ts_.currentLoc();
    Token nameToken = ts_.advance();
    LUC_LOG_TYPE("parseNamedType: consumed identifier '" << nameToken.value 
                 << "' at line " << nameToken.line << ", col " << nameToken.column);
    
    auto type = arena_.make<NamedTypeAST>(pool_.intern(nameToken.value));
    type->loc = loc;
    type->name = pool_.intern(nameToken.value);

    // Check for generic arguments
    LUC_LOG_TYPE("parseNamedType: next token after name: '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_TYPE("parseNamedType: found '<' for generic arguments at line " 
                     << ts_.peek().line << ", col " << ts_.peek().column);
        
        // IMPORTANT: parseGenericArgs expects the caller to have consumed the '<'
        // But our parseGenericArgs also expects the '<' to be already consumed!
        // Let's check how parseGenericArgs is implemented...
        
        type->genericArgs = parseGenericArgs();
        LUC_LOG_TYPE("parseNamedType: parsed " << type->genericArgs.size() 
                     << " generic argument(s)");
        LUC_LOG_TYPE("parseNamedType: after generic args, next token: '" 
                     << ts_.peek().value << "' (type=" << static_cast<int>(ts_.peek().type)
                     << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    }

    return type;
}