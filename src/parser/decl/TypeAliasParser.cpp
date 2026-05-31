#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

/**
 * @brief Parses a type alias declaration.
 * 
 * Grammar: `type` IDENTIFIER [ `<` generic_params `>` ] `=` type
 * 
 * Example: `type Transform<T> = (v T) -> T`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at 'type' keyword
 * On exit:  positioned after the aliased type
 * 
 * ─── Important Notes ───────────────────────────────────────────────────────
 *   - Does NOT create a new nominal type (unlike struct)
 *   - Alias is interchangeable with its target
 *   - Generic parameters allow instantiation with concrete types
 *   - Can be top‑level or local
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing alias name: returns nullptr
 * - Missing '=' after name/generics: reports error, returns nullptr
 * - Missing aliased type: reports error (node created with null aliasedType)
 */
ASTPtr<TypeAliasDeclAST> Parser::parseTypeAliasDecl(Visibility vis) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TYPE, "expected 'type' before type alias");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type alias name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);

    auto node = arena_.make<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    if (ts_.check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    ts_.consume(TokenType::ASSIGN, "expected '=' in type alias");

    node->aliasedType = parseType();
    if (!node->aliasedType) {
        errorAt(DiagCode::E2005, "expected type on the right-hand side of type alias");
    }

    return node;
}