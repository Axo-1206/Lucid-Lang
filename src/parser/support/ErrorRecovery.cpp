/**
 * @file ErrorRecovery.cpp
 * @brief Implementation of error recovery functions for the parser.
 * 
 * These functions provide panic-mode error recovery with bracket-aware
 * synchronization. They are used by the main parser to recover from
 * syntax errors.
 */

#include "ErrorRecovery.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace parser {

// =============================================================================
// synchronizeToBoundary - Decl/Stmt-Keyword-Aware Recovery
// =============================================================================

// See doc comment in ErrorRecovery.hpp. Unlike synchronizeTo(), this always 
// includes is_declaration_keyword()/is_statement_keyword() in the stop set, 
// so it can never skip past the start of the next declaration or statement - 
// only past tokens that belong to the current, already-broken production.
SyncResult synchronizeToBoundary(TokenStream& stream, ParserContext& ctx,
                                std::initializer_list<TokenType> extraStops) {
    return synchronizeUntil(stream, ctx, [&](TokenType t) {
        if (is_declaration_keyword(t) || is_statement_keyword(t)) {
            return true;
        }
        for (TokenType stop : extraStops) {
            if (t == stop) return true;
        }
        return false;
    });
}

} // namespace parser