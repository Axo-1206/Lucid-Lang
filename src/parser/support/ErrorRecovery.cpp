/**
 * @file ErrorRecovery.cpp
 * @brief The non-template error-recovery function.
 *
 * `synchronizeUntil` and `synchronizeTo` are templates and live in
 * ErrorRecovery.hpp. `synchronizeToBoundary` is not a template; its
 * implementation is here.
 */

#include "ErrorRecovery.hpp"

namespace lucid::parser {

SyncResult synchronizeToBoundary(TokenStream& stream,
                                 ParserContext& ctx,
                                 std::initializer_list<TokenType> extraStops) {
    return synchronizeUntil(stream, ctx, [&](TokenType t) {
        // The two keyword predicates come from Tokens.hpp. They cover
        // every token that can begin a declaration or a statement.
        if (isDeclarationKeyword(t) || isStatementKeyword(t)) {
            return true;
        }
        // The caller's construct-specific stop set.
        for (TokenType stop : extraStops) {
            if (t == stop) return true;
        }
        return false;
    });
}

} // namespace lucid::parser