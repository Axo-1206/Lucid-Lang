#include "../Parser.hpp"
#include "core/ast/BaseAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "ParserContext.hpp"

namespace parser {

// =============================================================================
// Error Recovery Functions
// =============================================================================

/**
 * @brief Skip tokens until `stopAt(currentTokenType)` is true, without
 * consuming or crossing a delimiter that belongs to an enclosing construct.
 *
 * This is panic-mode error recovery, but unlike a plain forward scan it is
 * bracket-aware: it tracks the delimiters (`(` `)`, `[` `]`, `{` `}`) opened
 * *while skipping* and only treats a closing delimiter as consumable if it
 * matches the most recently opened one. A closing delimiter that does not
 * match anything opened during the skip is assumed to belong to whatever
 * construct called this function — it is left unconsumed and the function
 * returns immediately, so the caller (not this function) decides what to
 * do with it.
 *
 * ## Why bracket TYPE matters, not just nesting depth
 *
 * A scalar depth counter ("increment on any opener, decrement on any
 * closer") cannot tell delimiter kinds apart, so it will happily consume
 * a `]` it thinks closes an unrelated stray `(`. For example, recovering
 * through `foo(1, (2 ]` with a naive depth counter incorrectly eats the
 * `]`, leaving the caller (e.g. `parseAttributes`, which owns that `]`)
 * with nothing left to consume for its own closing bracket. Tracking the
 * *expected* closer for each opener — not just a count — is what prevents
 * this: at `]`, the expected closer is `)` (from the stray `(`), so `]`
 * is correctly left alone.
 *
 * ## Stop condition semantics
 *
 * `stopAt` is only consulted when no delimiter opened during this call is
 * still awaiting its match (i.e. we are not "inside" something we opened
 * while skipping). This means `stopAt` will never fire on a token that is
 * lexically nested inside a bracketed region this function skipped over —
 * only on a token that is a true sibling of where skipping began.
 *
 * @tparam Predicate Callable with signature `bool(TokenType)`.
 * @param stream The token stream for the current file.
 * @param ctx The parsing context (used for logging; reserved for future
 *        diagnostics such as reporting the unmatched delimiter's location).
 * @param stopAt Predicate returning true for a token type that should end
 *        the skip. May combine literal comparisons and semantic category
 *        checks (e.g. `t == SEMICOLON || is_declaration_keyword(t)`) —
 *        this is why the parameter is a predicate rather than a fixed
 *        token list; see `synchronizeTo` for the fixed-list convenience
 *        wrapper built on top of this function.
 *
 * ## Postconditions
 *
 * - On success: the current token satisfies `stopAt` and has NOT been
 *   consumed — the caller decides whether/how to consume it.
 * - On reaching an enclosing, non-matching closer: the current token is
 *   that closer, NOT consumed, even if it happens to satisfy `stopAt`.
 * - On EOF: the stream is left at EOF; nothing further to consume.
 *
 * ## Example
 *
 * ```lucid
 * @[foo(1, (2 ]     -- stray unmatched '(' inside the attribute args
 * ```
 * ```cpp
 * // Recovering from the malformed argument list with
 * // synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN):
 * //   '(' → not a stop token, no closer expected yet → push RPAREN, skip
 * //   '2' → plain skip
 * //   ']' → does not match the expected RPAREN → left unconsumed, return
 * // Control returns to parseAttributes still positioned at ']', which is
 * // the only function that consumes it, exactly once.
 * ```
 *
 * ## Limitation: '<' and '>' are NOT tracked as brackets
 *
 * Only `(` `)`, `[` `]`, `{` `}` participate in expectedClosers — `<` and
 * `>` are deliberately excluded and are treated as ordinary tokens,
 * identical in status to an IDENTIFIER. This is not an oversight: unlike
 * the other three pairs, `<`/`>` are lexically ambiguous in Lucid (as in
 * C++) — `a<b>(c)` could be a generic call or `(a<b)>c` as chained
 * comparisons — so there is no reliable way for a token-level skip loop
 * to know whether a given `<` starts a real delimiter pair at all. See
 * the parser's own design discussion on generic-call-vs-comparison
 * disambiguation for why this can't be resolved without deeper lookahead
 * or backtracking, which this recovery function intentionally does not
 * attempt.
 *
 * The consequence: `<...>` never opens or closes anything as far as this
 * function is concerned, but that's harmless whenever it appears INSIDE
 * an already-open `(`/`[`/`{` region, because expectedClosers being
 * non-empty already suppresses `stopAt` on every token in between,
 * `<`/`>` included — see the worked example below.
 *
 * ```lucid
 * b [Point<T, U> get_default()], c int      -- inside a param list
 * ```
 * ```cpp
 * // Recovering with FuncParams' follow-set {COMMA, RPAREN, LBRACE, ...}:
 * //   '['              → opener, push RBRACKET, skip
 * //   'Point'          → expectedClosers non-empty → stopAt not even
 * //                       checked → plain skip
 * //   '<'              → not tracked as a bracket at all → plain skip
 * //   'T'              → plain skip
 * //   ','              → expectedClosers non-empty → NOT a stop, even
 * //                       though COMMA is in the follow-set — plain skip
 * //   'U', '>'         → plain skip ('>' untracked, same as '<')
 * //   'get_default'    → plain skip
 * //   '('              → opener, push RPAREN (stack: [RBRACKET, RPAREN])
 * //   ')'              → matches top (RPAREN) → pop, consume, continue
 * //   ']'              → matches top (RBRACKET) → pop, consume, continue
 * //   ','              → expectedClosers now EMPTY → stopAt(COMMA) →
 * //                       true → stop, unconsumed
 * // The whole '[Point<T, U> get_default()]' is skipped as one atomic
 * // unit — nested call, comma inside '<T, U>', and all — and recovery
 * // resurfaces exactly at the comma separating parameters.
 * ```
 */
template<typename Predicate>
void synchronizeUntil(TokenStream& stream, ParserContext& ctx, Predicate stopAt) {
    LOG_PARSER("Synchronizing");
    // NOTE: '<'/'>' intentionally absent — see this function's doc
    // comment ("Limitation: '<' and '>' are NOT tracked as brackets").
    std::vector<TokenType> expectedClosers;   // matches bracket TYPE, not just count

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
                // Closes something we opened while skipping — part of
                // the malformed region, consume it and keep scanning.
                expectedClosers.pop_back();
                stream.advance();
                continue;
            }
            if (expectedClosers.empty() && stopAt(current)) {
                LOG_PARSER_DETAIL("Synchronized at token: ", debug::tokenTypeToString(current));
                return;
            }
            // Doesn't match anything we opened, and isn't our stop token —
            // belongs to an enclosing construct (a "foreign closer" — see
            // this function's first doc example). Stop, don't consume.
            LOG_PARSER_DETAIL("Synchronization stopped before enclosing closer: ",
                               debug::tokenTypeToString(current));
            return;
        }

        // stopAt is deliberately gated on expectedClosers being empty:
        // this is what makes an entire opened region — including a
        // COMMA that would otherwise match the caller's own follow-set —
        // get skipped atomically instead of stopping partway through it.
        // See "Limitation: '<' and '>'" above for the worked trace.
        if (expectedClosers.empty() && stopAt(current)) {
            LOG_PARSER_DETAIL("Synchronized at token: ", debug::tokenTypeToString(current));
            return;
        }

        if (isOpener(current)) expectedClosers.push_back(matchingCloser(current));
        stream.advance();
    }

    LOG_PARSER("Synchronization reached EOF");
}

/**
 * @brief Synchronize to any of a fixed set of token types.
 *
 * Thin wrapper over synchronizeUntil — kept so existing call sites
 * (synchronizeTo(stream, ctx, TokenType::RBRACKET)) don't need to change,
 * while gaining depth-safety for free.
 */
template<typename... StopTokens>
void synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens) {
    synchronizeUntil(stream, ctx, [&](TokenType t) {
        return ((t == stopTokens) || ...);
    });
}

/**
 * @brief Synchronize using the follow-set implied by the parser's current
 * SyntacticContext, instead of a fixed/blind token set.
 *
 * Replaces the old untargeted synchronize(): rather than scanning for
 * one global set of "looks like a new statement" tokens regardless of
 * what's currently open, this looks at ctx.currentContext() (pushed by
 * ScopedContext) and picks the right follow-set for that construct.
 *
 * @return SyncOutcome — see its own doc comment. Callers that don't need
 * to distinguish the two (most don't — they just `return nullptr;`
 * afterward regardless of which token was hit) can ignore the return
 * value entirely; only a caller that loops and needs to know whether
 * it's safe to keep parsing more items (e.g. parseAttributes' main
 * loop) needs to check it.
 *
 * ## Why each follow-set has more than just "the matching closer"
 *
 * synchronizeUntil() is bracket-TYPE-aware (see its own doc comment): a
 * closer only stops the scan if it matches what was actually opened
 * while skipping. That solves the case where recovery runs INTO an
 * unrelated foreign closer. It does NOT solve the opposite case: recovery
 * running PAST a missing closer, by treating what comes after it as
 * legitimate nested content to skip over instead of a signal that the
 * closer was never written at all. Each follow-set below therefore
 * includes not just its own structural delimiter, but also whatever
 * token naturally follows this construct when it's well-formed — seeing
 * that token means "the closer that should have preceded this is
 * missing," not "descend further."
 *
 * Two concrete cases this fixes:
 *
 * ```lucid
 * func add(a int, b int {      -- missing ')' before '{'
 *     return a + b;
 * }
 * ```
 * A follow-set of just {COMMA, RPAREN} would treat '{' as an opener to
 * skip into, consume the whole function body looking for a ')' that
 * isn't coming, and only resurface after ANOTHER function's '{' or EOF.
 * Adding LBRACE to FuncParams' follow-set stops recovery right at the
 * missing ')' instead.
 *
 * ```lucid
 * let result = compute<int, string(5, "x");   -- missing '>' before '('
 * ```
 * Same failure shape: {COMMA, GREATER} treats the call's '(' as nested
 * content, consumes the whole call, then hunts for a '>' that's never
 * coming. Adding LPAREN to GenericArgs' follow-set (specifically the
 * call-site case, not the declaration-site GenericParams) stops at the
 * missing '>' instead.
 *
 * A third case can't be fixed by widening the bracket set at all, because
 * there's no bracket for bracket-matching to reason about:
 *
 * ```lucid
 * @[inline, deprecated("old")     -- missing ']'
 * let x int = 42;
 * ```
 * Nothing in `let x int = 42;` is a delimiter, so a purely
 * bracket-shaped follow-set has nothing to stop at and would run
 * indefinitely. This is why every context below also includes SEMICOLON
 * and is_declaration_keyword — the same semantic escape valve
 * StructBody/EnumBody/TraitBody/FuncBody/TopLevel already relied on —
 * so recovery always has a way out even when the surrounding tokens
 * happen to contain no brackets at all. Landing on one of these is
 * exactly what SyncOutcome::Abandoned reports back to the caller.
 */
SyncOutcome synchronizeToContext(TokenStream& stream, ParserContext& ctx) {
    switch (ctx.currentContext()) {
        case SyntacticContext::Attribute: {
            // @[...]. Structural: ',' (next attribute) or ']' (list ends).
            // Semantic escape: a stray declaration after a malformed
            // attribute, with no bracket in between to catch it (see the
            // @[inline, deprecated("old") \n let x ... example above).
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
            // Declaration site: struct Point<T> { ... } / func f<T>(...).
            // Structural: ',' or '>'. Natural continuation: '{' (struct/
            // trait/enum body) or '(' (function params) immediately after
            // a missing '>' — either signals the '>' was dropped, not
            // that we're still inside the generic parameter list.
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
            // Import site: compute<int, string>(...). Structural: ',' or
            // '>'. Natural continuation: '(' — a call's argument list
            // starting right where '>' should have closed the generic
            // args is exactly the missing-'>' case worked through above.
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
            // (a int, b int). Structural: ',' or ')'. Natural
            // continuation: '{' — a function body starting right where
            // ')' should have closed the parameter list is the missing-
            // ')' case worked through above.
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

        case SyntacticContext::FuncBody:
        case SyntacticContext::StructBody:
        case SyntacticContext::EnumBody:
        case SyntacticContext::TraitBody:
            // { ... } bodies. Structural: '}'. Semantic escape: ';' or
            // the start of a new member/statement — this was the
            // original case this design was built around (see
            // synchronizeUntil's own doc comment). No caller currently
            // needs to distinguish "hit '}'" from "hit the escape valve"
            // here — both mean "this body-parsing loop should stop" —
            // so this always reports Abandoned rather than adding a
            // distinction nothing consumes yet.
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::SEMICOLON
                    || t == TokenType::RBRACE
                    || is_declaration_keyword(t);
            });
            return SyncOutcome::Abandoned;

        case SyntacticContext::TopLevel:
        default:
            // Nothing bracketed is open at all — only the semantic
            // escape valve applies. Always Abandoned, for the same
            // reason as the body-context case above.
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::SEMICOLON || is_declaration_keyword(t);
            });
            return SyncOutcome::Abandoned;
    }
}

} // namespace parser