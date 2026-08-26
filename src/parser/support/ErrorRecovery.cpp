/**
 * @file ErrorRecovery.cpp
 * @brief Implementation of error recovery functions for the parser.
 * 
 * These functions provide panic-mode error recovery with bracket-aware
 * synchronization. They are used by the main parser to recover from
 * syntax errors.
 */

#include "../Parser.hpp"
#include "core/ast/BaseAST.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace parser {

// =============================================================================
// synchronizeUntil - Core Recovery Function
// =============================================================================

/**
 * @brief Skip tokens until stopAt() returns true, respecting bracket nesting.
 * 
 * Tracks opened brackets (`(`, `[`, `{`) and only stops when none are
 * open and stopAt() returns true. A closing bracket that doesn't match
 * the most recently opened bracket is left unconsumed (belongs to caller).
 * 
 * @tparam Predicate Callable with signature `bool(TokenType)`
 * @param stream The token stream
 * @param ctx The parsing context
 * @param stopAt Predicate that returns true for tokens that should end the skip
 * @return Which of the four SyncResult outcomes actually happened.
 */
template<typename Predicate>
SyncResult synchronizeUntil(TokenStream& stream, ParserContext& ctx, Predicate stopAt) {
    Trace::detail("Synchronizing");

    struct OpenBracket {
        TokenType closer;
        SourceLocation openedAt;
    };
    std::vector<OpenBracket> expectedClosers;
    
    auto isOpener = [](TokenType t) {
        return t == TokenType::LPAREN || t == TokenType::LBRACKET || t == TokenType::LBRACE;
    };
    auto isCloser = [](TokenType t) {
        return t == TokenType::RPAREN || t == TokenType::RBRACKET || t == TokenType::RBRACE;
    };
    auto matchingCloser = [](TokenType opener) {
        switch (opener) {
            case TokenType::LPAREN:   return TokenType::RPAREN;
            case TokenType::LBRACKET: return TokenType::RBRACKET;
            default:                  return TokenType::RBRACE;
        }
    };

    while (!stream.isAtEnd()) {
        TokenType current = stream.peekType();

        if (isCloser(current)) {
            if (!expectedClosers.empty() && expectedClosers.back().closer == current) {
                expectedClosers.pop_back();
                stream.consume();
                continue;
            }
            if (expectedClosers.empty() && stopAt(current)) {
                Trace::detail("Synchronized at: ", debug::tokenTypeToString(current));
                return SyncResult::Matched;
            }
            // Foreign closer - belongs to enclosing construct (or a genuine
            // bracket-kind mismatch in the source, e.g. `[1, 2}` - either way,
            // not ours to consume).
            Trace::detail("Stopped before enclosing closer: ",
                               debug::tokenTypeToString(current));
            return SyncResult::ForeignCloser;
        }

        if (expectedClosers.empty() && stopAt(current)) {
            Trace::detail("Synchronized at: ", debug::tokenTypeToString(current));
            return SyncResult::Matched;
        }

        if (isOpener(current)) {
            expectedClosers.push_back({matchingCloser(current), stream.currentLoc()});
        }
        stream.consume();
    }

    if (!expectedClosers.empty()) {
        Trace::detail("Synchronization reached EOF with ", expectedClosers.size(),
                      " unclosed bracket(s), innermost opened at line ",
                      expectedClosers.back().openedAt.line());
        return SyncResult::UnclosedBracket;
    }

    Trace::detail("Synchronization reached EOF");
    return SyncResult::ReachedEnd;
}

// =============================================================================
// synchronizeTo - Fixed Token Set Wrapper
// =============================================================================

template<typename... StopTokens>
SyncResult synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens) {
    return synchronizeUntil(stream, ctx, [&](TokenType t) {
        return ((t == stopTokens) || ...);
    });
}

// =============================================================================
// synchronizeToDeclBoundary - Decl/Stmt-Keyword-Aware Recovery
// =============================================================================

// See doc comment in Parser.hpp. Unlike synchronizeTo(), this always includes
// is_declaration_keyword()/is_statement_keyword() in the stop set, so it can
// never skip past the start of the next declaration or statement - only past
// tokens that belong to the current, already-broken production.
SyncResult synchronizeToDeclBoundary(TokenStream& stream, ParserContext& ctx,
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

// =============================================================================
// Explicit Template Instantiation
// =============================================================================

// Explicitly instantiate the common sync patterns used by the parser
template SyncResult synchronizeTo(TokenStream&, ParserContext&, TokenType);
template SyncResult synchronizeTo(TokenStream&, ParserContext&, TokenType, TokenType);
template SyncResult synchronizeTo(TokenStream&, ParserContext&, TokenType, TokenType, TokenType);
template SyncResult synchronizeTo(TokenStream&, ParserContext&, TokenType, TokenType, TokenType, TokenType);

} // namespace parser