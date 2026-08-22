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
#include "debug/DebugMacros.hpp"
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
 */
template<typename Predicate>
void synchronizeUntil(TokenStream& stream, ParserContext& ctx, Predicate stopAt) {
    Trace::detail("Synchronizing");
    
    std::vector<TokenType> expectedClosers;
    
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
            if (!expectedClosers.empty() && expectedClosers.back() == current) {
                expectedClosers.pop_back();
                stream.consume();
                continue;
            }
            if (expectedClosers.empty() && stopAt(current)) {
                Trace::detail("Synchronized at: ", debug::tokenTypeToString(current));
                return;
            }
            // Foreign closer - belongs to enclosing construct
            Trace::detail("Stopped before enclosing closer: ",
                               debug::tokenTypeToString(current));
            return;
        }

        if (expectedClosers.empty() && stopAt(current)) {
            Trace::detail("Synchronized at: ", debug::tokenTypeToString(current));
            return;
        }

        if (isOpener(current)) {
            expectedClosers.push_back(matchingCloser(current));
        }
        stream.consume();
    }

    Trace::detail("Synchronization reached EOF");
}

// =============================================================================
// synchronizeTo - Fixed Token Set Wrapper
// =============================================================================

template<typename... StopTokens>
void synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens) {
    synchronizeUntil(stream, ctx, [&](TokenType t) {
        return ((t == stopTokens) || ...);
    });
}

// =============================================================================
// synchronizeToContext - Context-Aware Recovery
// =============================================================================

SyncOutcome synchronizeToContext(TokenStream& stream, ParserContext& ctx) {
    switch (ctx.currentContext()) {
        case SyntacticContext::Attribute: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::COMMA
                    || t == TokenType::RBRACKET
                    || t == TokenType::SEMICOLON
                    || is_declaration_keyword(t);
            });
            if (!stream.isAtEnd()) {
                TokenType t = stream.peekType();
                if (t == TokenType::COMMA || t == TokenType::RBRACKET) {
                    return SyncOutcome::Continuable;
                }
            }
            return SyncOutcome::Abandoned;
        }

        case SyntacticContext::GenericParams: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::COMMA
                    || t == TokenType::GREATER
                    || t == TokenType::LBRACE
                    || t == TokenType::LPAREN
                    || t == TokenType::SEMICOLON
                    || is_declaration_keyword(t);
            });
            if (!stream.isAtEnd()) {
                TokenType t = stream.peekType();
                if (t == TokenType::COMMA || t == TokenType::GREATER) {
                    return SyncOutcome::Continuable;
                }
            }
            return SyncOutcome::Abandoned;
        }

        case SyntacticContext::GenericArgs: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::COMMA
                    || t == TokenType::GREATER
                    || t == TokenType::LPAREN
                    || t == TokenType::SEMICOLON
                    || is_declaration_keyword(t);
            });
            if (!stream.isAtEnd()) {
                TokenType t = stream.peekType();
                if (t == TokenType::COMMA || t == TokenType::GREATER) {
                    return SyncOutcome::Continuable;
                }
            }
            return SyncOutcome::Abandoned;
        }

        case SyntacticContext::FuncParams: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::COMMA
                    || t == TokenType::RPAREN
                    || t == TokenType::LBRACE
                    || t == TokenType::SEMICOLON
                    || is_declaration_keyword(t);
            });
            if (!stream.isAtEnd()) {
                TokenType t = stream.peekType();
                if (t == TokenType::COMMA || t == TokenType::RPAREN) {
                    return SyncOutcome::Continuable;
                }
            }
            return SyncOutcome::Abandoned;
        }

        // ─── SwitchBody: Special recovery for switch statements ──────────
        // We want to stop at 'case', 'default', or '}' (closing brace)
        // This allows us to continue parsing the switch body even after errors
        case SyntacticContext::SwitchBody: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::CASE
                    || t == TokenType::DEFAULT
                    || t == TokenType::RBRACE
                    || t == TokenType::SEMICOLON  // Skip stray semicolons, but don't stop on them
                    || is_declaration_keyword(t);
            });
            // For SwitchBody, we can always continue if we find case/default/RBRACE
            if (!stream.isAtEnd()) {
                TokenType t = stream.peekType();
                if (t == TokenType::CASE || t == TokenType::DEFAULT || t == TokenType::RBRACE) {
                    return SyncOutcome::Continuable;
                }
            }
            return SyncOutcome::Abandoned;
        }

        case SyntacticContext::FuncBody:
        case SyntacticContext::StructBody:
        case SyntacticContext::EnumBody:
        case SyntacticContext::TraitBody: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::SEMICOLON
                    || t == TokenType::RBRACE
                    || is_declaration_keyword(t);
            });
            return SyncOutcome::Abandoned;
        }

        case SyntacticContext::TopLevel:
        default: {
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::SEMICOLON || is_declaration_keyword(t);
            });
            return SyncOutcome::Abandoned;
        }
    }
}

// =============================================================================
// Explicit Template Instantiation
// =============================================================================

// Explicitly instantiate the common sync patterns used by the parser
template void synchronizeTo(TokenStream&, ParserContext&, TokenType);
template void synchronizeTo(TokenStream&, ParserContext&, TokenType, TokenType);
template void synchronizeTo(TokenStream&, ParserContext&, TokenType, TokenType, TokenType);
template void synchronizeTo(TokenStream&, ParserContext&, TokenType, TokenType, TokenType, TokenType);

} // namespace parser