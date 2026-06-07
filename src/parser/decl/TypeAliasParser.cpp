#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

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
 *   - Generic array syntax [_, <T>] is NOT valid here - use the declared
 *     generic parameter instead: `type List<T> = [_, T]`
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing alias name: returns nullptr
 * - Missing '=' after name/generics: reports error, returns nullptr
 * - Missing aliased type: reports error (node created with null aliasedType)
 */
TypeAliasDeclPtr Parser::parseTypeAliasDecl(Visibility vis) {
    LUC_LOG_DECL_VERBOSE("parseTypeAliasDecl: entering");
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::TYPE, "expected 'type' before type alias");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseTypeAliasDecl: ERROR - expected type alias name");
        errorAt(DiagCode::E1003, "expected type alias name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseTypeAliasDecl: alias name = " << pool_.lookup(name));

    auto node = arena_.make<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = name;
    node->visibility = vis;

    // Parse generic parameters at the alias level (e.g., `type List<T>`)
    if (ts_.check(TokenType::LESS)) {
        LUC_LOG_DECL_EXTREME("parseTypeAliasDecl: parsing generic parameters");
        node->genericParams = parseGenericParams();
        LUC_LOG_DECL_EXTREME("parseTypeAliasDecl: " << node->genericParams.size() 
                             << " generic parameter(s)");
    }

    ts_.consume(TokenType::ASSIGN, "expected '=' in type alias");

    // Parse the aliased type using parseArrayType() directly
    // Note: Generic array syntax [_, <T>] is NOT valid here and will be rejected
    // by parseArrayType() with an error message.
    node->aliasedType = parseType();
    if (!node->aliasedType) {
        LUC_LOG_DECL("parseTypeAliasDecl: ERROR - expected type on right-hand side");
        errorAt(DiagCode::E1005, "expected type on the right-hand side of type alias");
    } else {
        LUC_LOG_DECL_EXTREME("parseTypeAliasDecl: aliased type parsed");
        
        // Optional: check for misplaced generic array syntax
        if (node->aliasedType->isa<GenericArrayTypeAST>()) {
            errorAt(DiagCode::E1024, 
                    "generic array syntax '[_, <T>]' is not allowed in type alias right-hand side. "
                    "Use the declared generic parameter instead: 'type " 
                    + std::string(pool_.lookup(name)) + "<T> = [_, T]'");
        }
    }

    LUC_LOG_DECL_VERBOSE("parseTypeAliasDecl: success");
    return node;
}