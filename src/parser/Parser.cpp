/**
 * @file Parser.cpp
 * @brief Implementation of core parsing functions.
 * 
 * This file implements the core parsing infrastructure:
 * - TokenStream: Safe token consumption with comment skipping
 * - ParserContext: Shared context across all files
 * - Error Recovery: synchronizeUntil(), synchronizeTo(), synchronizeToContext()
 * - parse(): Single entry point - parses the root file and all imports
 * - parseInternal(): Parses internal declarations of a file
 * - parseImportDecl(): Parses import declaration and imports the module
 */

#include "Parser.hpp"
#include "lexer/Lexer.hpp"
#include "core/ast/BaseAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "support/ParserContext.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

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

// =============================================================================
// parse() - ENTRY POINT
// =============================================================================

/**
 * @brief Parse a single source file into a ModuleAST, recursively resolving
 * its `import` imports as needed.
 *
 * This is the ONE and ONLY entry point for parsing — the driver calls it
 * once for main.luc, and it is re-entered recursively (via parseImportDecl)
 * once per not-yet-parsed imported file.
 *
 * ## Model: modules, not a flat merge
 *
 * Each call returns a ModuleAST containing only THIS file's own top-level
 * declarations. Imported files are NOT inlined/flattened into it — a
 * `import path [as alias]` becomes a UseDeclAST that stores a reference
 * (path + alias) to another module. That referenced module is resolved to
 * its own ModuleAST separately, via ModuleResolver's cache, and symbol
 * lookup across the reference (e.g. `math.sqrt`) is the semantic/binder
 * pass's job, not parse()'s. This preserves per-file namespacing — two
 * modules may each declare a `Point` without colliding — which a flat,
 * single-Program merge would not.
 *
 * The "whole program" is therefore not one object parse() builds and
 * returns. It's the graph formed by the root ModuleAST plus every module
 * transitively reachable from it through ModuleResolver's cache. The
 * driver gets the full set, in a dependency-safe order, via
 * `ctx.resolver->getModuleOrder()` after the root parse() call returns —
 * see below.
 *
 * ## Every attempted file produces a real ModuleAST — never nullptr
 *
 * parse() always returns a non-null ModuleAST*, even for a file that
 * couldn't be meaningfully parsed at all (empty/failed lex, a circular
 * import). Check `ModuleAST::hasErrors` to tell a clean parse from one
 * that didn't fully succeed — don't check the return value for null.
 * This is what lets callers (parseImportDecl in particular) always call
 * parse() unconditionally, without a nullptr branch to handle.
 *
 * The one exception: a circular import returns an uncached dummy (see
 * step 3 below) rather than nothing, specifically so the in-progress
 * parse further up the call stack — which owns the real module — is
 * left alone to finish and cache the actual result.
 *
 * ## Flow
 *
 * ```
 * 1. Driver calls parse(main.luc, source, ctx)
 *       → the one true entry point; re-entered recursively for every `import`
 *
 * 2. parse(path, source, ctx):
 *    a. Intern path
 *    b. Cache check — resolver->getParsedModule(filePath):
 *         HIT  → return the cached ModuleAST immediately, done. This is
 *                what makes "always call parse()" safe for callers: a
 *                second `import` of an already-attempted file (successful
 *                or not) never re-lexes/re-parses it.
 *         MISS → continue below
 *    c. ctx.currentFilePath = path
 *    d. ScopedFileContext   — save this file's caller's contextStack and
 *                             error-tracking fields, reset both for this
 *                             file (this file starts at TopLevel with no
 *                             errors yet), restore the caller's on return
 *                             (draining this file's own errors into
 *                             ctx.allDiagnostics first)
 *    e. Circular-import check — resolver->isParsing(filePath):
 *         if already on the "in progress" stack → build an uncached
 *         dummy ModuleAST (hasErrors = true), return it immediately.
 *         No diagnostic here — parseImportDecl checks isParsing() itself
 *         before calling parse(), and reports it there, against the
 *         `import` statement's real location.
 *    f. ScopedParsingGuard  — push filePath onto the "in progress" stack
 *    g. Lex source → tokens
 *         empty, or contains an UNKNOWN token → build a ModuleAST with
 *         hasErrors = true and no decls, cache it, return it (still a
 *         real, cached module — not nullptr, not a special case for
 *         callers to handle)
 *    h. Construct TokenStream; loop parseDecl() until EOF     ─┐
 *         │                                                    │ step 3
 *         └── every declaration goes into this file's own list │
 *    i. Build ModuleAST from that decl list; errors = ctx.errors (copied
 *       while still populated), hasErrors = ctx.hasErrors
 *    j. resolver->cacheModule(path, thisModule) — unconditional now,
 *       including when hasErrors is true; see step 2's cache check for
 *       why a broken module still needs to be cached, not just skipped
 *    k. [guards destruct: pop "in progress" stack, restore caller's
 *       contextStack/errors]
 *    l. return thisModule
 *
 * 3. Inside the loop (2h), whenever parseDecl() hits a
 *    `import path [as alias]`:
 *    a. Resolve `path` → file path
 *    b. If resolver->isParsing(filePath) → report circular-import error
 *       here (this location, not parse()'s)
 *    c. Call parse(filePath, source, ctx) unconditionally — cache-checked
 *       internally at step 2b, so this never redoes real work
 *    d. UseDeclAST stores {path, alias} — a REFERENCE to the module,
 *       not its inlined content or a pointer to it
 *    e. Control returns to 3, continues the CURRENT file's loop at 2h
 *
 * 4. Root parse() call returns the root ModuleAST.
 *    The full "program" = ctx.resolver->getModuleOrder(), a flat list of
 *    every attempted module's path in POST-order (completion order): a
 *    module is appended the moment its own parse() finishes, which is
 *    after everything it `import`s has already finished and been appended.
 *    The root/main module — depending, transitively, on everything else
 *    — is therefore always LAST. This is what makes the list safe for
 *    single-pass semantic analysis: walking it front-to-back, everything
 *    to the left of a given module has already been fully processed.
 * ```
 *
 * @param path The file path
 * @param source The source code
 * @param ctx The parsing context (shared across all files)
 * @return ModuleAST* Never nullptr. This file's own declarations only —
 *         does NOT include imported files' declarations; see them via
 *         their own ModuleAST, reachable through ctx.resolver. Check
 *         `->hasErrors` rather than the pointer to detect failure.
 *
 * ## Three RAII guards, three disjoint pieces of state
 *
 * This function constructs a ScopedFileContext and a ScopedParsingGuard
 * back to back (step 2d and 2f/3f below), and every parse function it
 * calls into may construct ScopedContext internally (e.g. parseAttributes,
 * a struct/enum/trait/func body parser). It's easy to assume three RAII
 * guards showing up around the same call must overlap in what they do —
 * they don't. Each guards a different member of a different object, at a
 * different granularity, and none of them reads or writes what either of
 * the others touches:
 *
 * | Guard                | Guards                                                                       | Lives on                        | Frequency per file                               | Answers                                                                |
 * | -------------------- | ---------------------------------------------------------------------------- | ------------------------------- | -------------------------------------------------| -----------------------------------------------------------------------|
 * | `ScopedContext`      | one `ContextFrame` (push one, pop one)                                       | `ParserContext::contextStack`   | many — one per attribute list, generic-arg list, | "What syntactic construct is the parser                                |
 * |                      |                                                                              |                                 | function/struct/enum/trait body, etc.            | inside *right now*, within this file?"                                 |
 * | `ScopedFileContext`  | the whole `contextStack`, plus `errors` / `hasErrors` /                      | `ParserContext`                 | exactly one — the whole file is one activation   | "Whose file's syntax-nesting/error                                     |
 * |                      | `consecutiveErrors` (save → reset → restore, draining into `allDiagnostics`) |                                 |                                                  |  state is currently live in `ctx`?"                                    |
 * | `ScopedParsingGuard` | one entry in `parsingStack_` (push one path, pop one path)                   | `ModuleResolver::parsingStack_` | exactly one — the whole file is one activation   | "Which file paths are currently mid-parse,                             |
 * |                      |                                                                              |                                 |                                                  |  up the *entire* call stack (including files that imported this one)?" |
 *
 * `ScopedContext` frames nest *inside* a single `ScopedFileContext`
 * activation, potentially many levels deep, over the course of one file —
 * see ScopedContext's own doc comment in ParserContext.hpp. `ScopedFileContext`
 * and `ScopedParsingGuard` are both constructed once per `parse()` call
 * (steps 2d and 2f/3f) precisely because a single file-parsing activation
 * genuinely needs both kinds of isolation at once: reset this file's own
 * syntax/error bookkeeping so it doesn't see the importer's leftover state
 * (`ScopedFileContext`), *and* mark this file's path as "in progress" so a
 * nested `import` back to it is caught as a cycle rather than infinite
 * recursion (`ScopedParsingGuard`). They sit on entirely different objects
 * (`ParserContext ctx` vs. `ModuleResolver* ctx.resolver`) — dropping either
 * one breaks a different thing, which is the tell that they were never
 * redundant with each other in the first place.
 */
ModuleAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx) {
    LOG_PARSER_MINIMAL("Parsing file: ", path);
    
    // ─── 1. Intern the file path ────────────────────────────────────────
    InternedString filePath = ctx.pool.intern(path);

    // ─── 2. Cache check — parse() is safe to call unconditionally ───────
    // Callers (parseImportDecl in particular) always call parse() rather than
    // checking the cache themselves first; this is where that's actually
    // honored without redoing real work. A module that already finished —
    // successfully or not; see ModuleAST::hasErrors — is never re-lexed
    // or re-parsed just because a second file also `import`s it.
    if (ctx.resolver) {
        if (auto* cached = ctx.resolver->getParsedModule(filePath)) {
            return cached;
        }
    }

    ctx.currentFilePath = filePath;

    // Save the importing file's contextStack and error-tracking fields
    // (if any), reset both for this file, and restore them on return —
    // on every exit path below, including the early returns. This also
    // owns the clearErrors() reset directly; see ScopedFileContext's doc
    // comment for why that can't safely be a separate call site anymore.
    ScopedFileContext fileContext(ctx);
    
    // ─── 3. Check for Cyclic Dependencies ───────────────────────────────
    // We put the file that currently parsing into a stack, if the next
    // file is already in the stack then it's a Cyclic Dependencies.
    //
    // No diagnostic is reported here on purpose: this function has no
    // meaningful source location for a file that's already mid-parse
    // further up the call stack. parseImportDecl checks isParsing() itself,
    // before calling parse(), and reports the error there against the
    // `import` statement's own location. This branch only needs to return a
    // real (but deliberately uncached) ModuleAST so the in-progress parse
    // further up the stack can finish and cache the actual result.
    if (ctx.resolver && ctx.resolver->isParsing(filePath)) {
        LOG_PARSER("Circular import detected, returning uncached dummy: ", path);
        auto* dummy = ctx.arena.make<ModuleAST>();
        dummy->filePath = filePath;
        dummy->hasErrors = true;
        return dummy;
    }
    // Push filePath onto the parsing stack for cycle detection; popped
    // automatically on every return path below, including the lex-failure
    // early returns further down.
    ScopedParsingGuard parsingGuard(ctx.resolver, filePath);
    
    // ─── 4. Lex the Source ──────────────────────────────────────────────
    auto tokens = lexer::tokenize(source, path);

    if (tokens.empty()) {
        LOG_PARSER_MINIMAL("Lexer produced no tokens for: ", path);
        auto* mod = ctx.arena.make<ModuleAST>();
        mod->filePath = filePath;
        mod->hasErrors = false; // Empty file is not consider an error
        if (ctx.resolver) ctx.resolver->cacheModule(filePath, mod);
        return mod;
    }

    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            LOG_PARSER_MINIMAL("Lexer error in: ", path);
            // E0105 "Unknown character." — lexical-category fit; zero %s
            // placeholders in its template, so no args (tok.value is only
            // recoverable from the location, not the message text, given
            // the current message set).
            ctx.errorAt(SourceLocation(tok.line, tok.column), DiagCode::E0105);
            auto* mod = ctx.arena.make<ModuleAST>();
            mod->filePath = filePath;
            mod->errors = ctx.errors;    // this file's diagnostics (just the E0105 above,
                                          // plus anything earlier) — copied now, before
                                          // ScopedFileContext's destructor drains ctx.errors
                                          // into ctx.allDiagnostics and restores the
                                          // importer's own error list.
            mod->hasErrors = true;
            if (ctx.resolver) ctx.resolver->cacheModule(filePath, mod);
            return mod;
        }
    }
    
    // ─── 5. Create TokenStream ──────────────────────────────────────────
    TokenStream stream(std::move(tokens), filePath);
    
    // ─── 6. Parse Internal Declarations ─────────────────────────────────
    // parseInternal() is void — it always retains whatever declarations it
    // managed to collect before stopping, fatal threshold or not, rather
    // than signaling failure and discarding partial progress. This file
    // still produces a real ModuleAST either way; see its own doc comment.
    std::vector<DeclPtr> allDecls;
    parseInternal(stream, ctx, allDecls);
    
    // ─── 7. Build this file's ModuleAST ──────────────────────────────────
    auto* thisModule = ctx.arena.make<ModuleAST>();
    thisModule->filePath = filePath;
    
    auto builder = ctx.arena.makeBuilder<DeclPtr>();
    for (auto* d : allDecls) {
        builder.push_back(d);
    }
    thisModule->decls = builder.build();
    thisModule->errors = ctx.errors;    // this file's own diagnostics — copied now, while
                                         // ctx.errors still holds them, before
                                         // ScopedFileContext's destructor drains them into
                                         // ctx.allDiagnostics and restores the importer's
                                         // own error list (see this function's own doc
                                         // comment table above for why that restore
                                         // doesn't touch this module's copy).
    thisModule->hasErrors = ctx.hasErrors;
    
    // ─── 8. Cache the result ─────────────────────────────────────────────
    // Cached unconditionally now — including when thisModule->hasErrors is
    // true. This is what lets a second `import` of a broken file hit the
    // cache-check in step 2 instead of re-lexing/re-parsing it from
    // scratch; error status travels with the module via hasErrors instead
    // of being encoded as "not present in the cache."
    // (parsingGuard pops this file off the parsing stack automatically
    // when parse() returns, below — no manual popParsing() needed here.)
    if (ctx.resolver) {
        ctx.resolver->cacheModule(filePath, thisModule);
        LOG_PARSER_DETAIL("Cached module: ", path);
    }
    
    LOG_PARSER_MINIMAL("Parse completed: ", allDecls.size(), " total declarations");
    
    return thisModule;
}

// =============================================================================
// parseProgram() - Whole-program convenience wrapper over parse()
// =============================================================================

/**
 * @brief Parse the root file, then read back every module `parse()` visited.
 *
 * See this function's own doc comment in Parser.hpp for the full design
 * rationale (why this sits on top of `parse()` rather than changing
 * `parse()`'s own signature). Implementation is intentionally small: all
 * the real work already happened inside `parse()`/`ModuleResolver` by the
 * time this function's second half runs.
 */
std::vector<ModuleAST*> parseProgram(const std::string& rootPath,
                                      const std::string& rootSource,
                                      ParserContext& ctx) {
    // ─── 1. Parse the root file (recurses through every `import`) ───────
    ModuleAST* root = parse(rootPath, rootSource, ctx);

    // ─── 2. No resolver → no import graph to walk ────────────────────────
    // Every module `parse()` could have visited beyond the root file itself
    // is only tracked via ctx.resolver (see step 2/3/8 of parse()'s own
    // doc comment, all `if (ctx.resolver)`-guarded) — so without one, the
    // root module IS the whole program as far as this call can know.
    if (!ctx.resolver) {
        LOG_PARSER_MINIMAL("parseProgram: no resolver, returning root module only");
        return { root };
    }

    // ─── 3. Read back the resolver's own cache, in dependency order ─────
    // getModuleOrder() already guarantees: for any module M, every module
    // M imports appears before M — so this vector is immediately safe to
    // walk front-to-back for single-pass semantic analysis (see
    // ModuleResolver::getModuleOrder()'s own doc comment). Nothing here
    // re-derives or re-checks that ordering; it is simply read back.
    const auto& order = ctx.resolver->getModuleOrder();
    std::vector<ModuleAST*> modules;
    modules.reserve(order.size());
    for (InternedString path : order) {
        if (ModuleAST* mod = ctx.resolver->getParsedModule(path)) {
            modules.push_back(mod);
        }
        // A path in getModuleOrder() with no corresponding cached module
        // would mean cacheModule() and moduleOrder_ desynced — see
        // ModuleResolver::cacheModule()'s own invariant. Not expected;
        // silently skipped rather than asserted here, so a release build
        // still returns the modules it CAN account for.
    }

    LOG_PARSER_MINIMAL("parseProgram: ", modules.size(), " module(s) in program");
    return modules;
}

// =============================================================================
// parseInternal() - Parse a file's internal declarations
// =============================================================================

/**
 * @brief Parse the internal declarations of a single file.
 * 
 * This function is the core of the parser's file-level processing. It consumes
 * tokens from the stream and produces a collection of top-level declarations.
 * 
 * ## Program Flow
 * 
 * ```
 * parseInternal()
 *     │
 *     ├── 1. Setup Phase
 *     │   ├── Log entry
 *     │   └── Initialize tracking variables
 *     │       ├── declCount = 0
 *     │       ├── consecutiveFailures = 0
 *     │       └── lastPos = stream.getPos()
 *     │
 *     ├── 2. Main Parsing Loop
 *     │   │
 *     │   └── while (!stream.isAtEnd() && consecutiveFailures < MAX)
 *     │       │
 *     │       ├── 2.1 Harvest Doc Comment
 *     │       │   └── harvestDocComment() → scans backward for attached comments
 *     │       │
 *     │       ├── 2.2 Skip Stray Semicolons
 *     │       │   └── if (stream.check(SEMICOLON)) → advance() and continue
 *     │       │
 *     │       ├── 2.3 Parse Declaration
 *     │       │   └── parseDecl()
 *     │       │       │
 *     │       │       ├── IMPORT → parseImportDecl()
 *     │       │       │   └── Imports module (recursive)
 *     │       │       │
 *     │       │       ├── STRUCT → parseStructDecl()
 *     │       │       │
 *     │       │       ├── ENUM → parseEnumDecl()
 *     │       │       │
 *     │       │       ├── TRAIT → parseTraitDecl()
 *     │       │       │
 *     │       │       └── LET/CONST → parseVarDecl() or parseFuncDecl()
 *     │       │
 *     │       ├── 2.4 Progress Check
 *     │       │   │
 *     │       │   ├── if (stream.getPos() == savedPos)
 *     │       │   │   ├── No progress → consecutiveFailures++
 *     │       │   │   ├── Log stuck token
 *     │       │   │   ├── Force consume token if not at EOF
 *     │       │   │   └── if (consecutiveFailures > 5)
 *     │       │   │       └── synchronizeToContext() → aggressive recovery
 *     │       │   │
 *     │       │   ├── else if (decl != nullptr)
 *     │       │   │   ├── declCount++
 *     │       │   │   ├── consecutiveFailures = 0
 *     │       │   │   ├── lastPos = stream.getPos()
 *     │       │   │   ├── Attach doc comment if present
 *     │       │   │   └── outDecls.push_back(decl)
 *     │       │   │
 *     │       │   └── else
 *     │       │       └── consecutiveFailures = 0
 *     │       │           (made progress but returned nullptr)
 *     │       │
 *     │       └── 2.5 Critical Stuck Detection
 *     │           └── if (stream.getPos() == lastPos && consecutiveFailures > 10)
 *     │               ├── Log critical stuck
 *     │               ├── Force consume token
 *     │               └── lastPos = stream.getPos()
 *     │
 *     ├── 3. Error Handling
 *     │   │
 *     │   └── if (consecutiveFailures >= MAX)
 *     │       ├── ctx.error(stream, DiagCode::E0002, MAX)
 *     │       └── return  (outDecls keeps whatever was collected so far —
 *     │                     no signal is sent up; parse() still builds a
 *     │                     real ModuleAST and flags it via ctx.hasErrors)
 *     │
 *     └── 4. Return
 *         └── LOG_PARSER_MINIMAL("Parsed N declarations")
 * ```
 * 
 * ## Error Recovery Strategy
 * 
 * The parser uses a multi-level error recovery strategy:
 * 
 * ### Level 1: Per-Declaration Recovery
 * - Each declaration parser attempts to recover from local errors
 * - Returns nullptr on failure, allowing the loop to continue
 * 
 * ### Level 2: Progress Detection
 * - If the stream position doesn't advance, we force consume a token
 * - Prevents infinite loops on malformed input
 * 
 * ### Level 3: Panic Recovery
 * - After 5 consecutive failures, call `synchronizeToContext()`
 * - Skips tokens until the follow-set implied by the current
 *   SyntacticContext is reached (a declaration boundary at TopLevel,
 *   a matching delimiter inside a bracketed construct, etc.), without
 *   crossing a delimiter owned by an enclosing construct
 * 
 * ### Level 4: Abort
 * - After 100 consecutive failures, abort parsing
 * - Prevents infinite loops in pathological cases
 * 
 * ## Declaration Collection
 * 
 * All successfully parsed declarations are collected into `outDecls`:
 * - Each declaration has its doc comment attached (if any)
 * - Declarations are stored in source order
 * - Imported modules are parsed recursively via `parseImportDecl()`
 * 
 * ## Token Stream State
 * 
 * After this function completes, the token stream will be at:
 * - EOF (normal case)
 * - A recovery point (if errors occurred)
 * - The position where parsing was aborted (fatal error)
 * 
 * ## Thread Safety
 * 
 * Not thread-safe. Requires exclusive access to the token stream.
 * 
 * @param stream The token stream for the file being parsed.
 *              This stream is consumed during parsing.
 * @param ctx The parsing context (shared across all files).
 *           Contains error tracking, allocators, and module resolver.
 * @param outDecls Output vector to collect successfully parsed declarations.
 *                Each declaration will have its doc comment attached.
 *                The vector is cleared by the caller before calling this function.
 *                Always contains whatever was successfully collected, even
 *                if the fatal-failure threshold was hit — see below.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * std::vector<DeclPtr> decls;
 * parseInternal(stream, ctx, decls);
 * // decls contains everything collected, whether or not ctx.hasErrors —
 * // parse() always builds a ModuleAST from it and flags errors via
 * // ModuleAST::hasErrors, rather than this function signaling failure.
 * ```
 * 
 * @note This function does NOT handle imports directly. Imports are handled
 *       by parseImportDecl() which is called from parseDecl().
 *       The imported module's declarations are collected recursively
 *       when parseImportDecl() calls parse() on the imported file.
 * 
 * @note This function has no return value on purpose: every file that is
 *       attempted must produce a real ModuleAST (see parse()'s design),
 *       including one that hit the consecutive-failure threshold partway
 *       through. Check ctx.hasErrors (or the resulting ModuleAST::hasErrors)
 *       to tell a clean parse from one that stopped early, not a boolean
 *       return from this function.
 */
void parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclPtr>& outDecls) {
    LOG_PARSER_MINIMAL("Parsing internal declarations of: ", 
                       debug::internedToString(ctx.pool, ctx.currentFilePath));
    
    int declCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 100;
    size_t lastPos = stream.getPos();
    
    while (!stream.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES) {
        auto doc = harvestDocComment(stream, ctx);
        size_t savedPos = stream.getPos();
        
        // Skip stray semicolons
        if (stream.check(TokenType::SEMICOLON)) {
            LOG_PARSER_DETAIL("Skipping stray semicolon at top level");
            stream.advance();
            continue;
        }
        
        // ─── Parse a top-level declaration ──────────────────────────────
        // The declaration parser will consume its own terminating ';'
        auto* decl = parseDecl(stream, ctx);
        
        // ─── Check if we're at EOF after parsing a declaration ──────────
        // If we parsed a declaration and then hit EOF, but the declaration
        // was incomplete (e.g., missing closing brace), we should have
        // already reported an error. But if we're at EOF and the declaration
        // is nullptr, we might have a malformed declaration.
        if (!decl && stream.isAtEnd()) {
            // We reached EOF while trying to parse a declaration.
            // This could be because we're at EOF after a comment, or
            // there's a malformed declaration.
            // If we have any errors, break out.
            if (ctx.hasErrors) {
                break;
            }
            // No error was reported, but we have no declaration and we're at EOF.
            // This could be valid (trailing whitespace after last declaration).
            // Let's break out of the loop gracefully.
            LOG_PARSER_DETAIL("Reached EOF after parsing declarations");
            break;
        }
        
        // Check for progress
        if (stream.getPos() == savedPos) {
            consecutiveFailures++;
            LOG_PARSER("NO PROGRESS - stuck on token: ", stream.peek().value,
                       " type: ", debug::tokenTypeToString(stream.peekType()));
            
            if (!stream.isAtEnd()) {
                stream.advance();
            }
            
            if (consecutiveFailures > 5) {
                LOG_PARSER("Too many consecutive failures, aggressive recovery");
                // No ScopedContext should be open here — parseDecl's own
                // constructs (attributes, generics, bodies) pop themselves
                // on every return path before control gets back up to this
                // loop. currentContext() is therefore TopLevel, so this
                // scans for the next ';' or declaration-starting keyword
                // rather than a blind, unbounded token set.
                synchronizeToContext(stream, ctx);
            }
        } else if (decl) {
            declCount++;
            consecutiveFailures = 0;
            lastPos = stream.getPos();
            
            LOG_PARSER_DETAIL("Parsed declaration #", declCount, " (",
                              debug::kindToString(decl->kind), ")");
            
            if (doc) {
                decl->doc = std::move(doc);
            }
            outDecls.push_back(decl);
        } else {
            consecutiveFailures = 0;
            LOG_PARSER("parseDecl returned nullptr but made progress");
        }
        
        // Critical stuck detection
        if (stream.getPos() == lastPos && consecutiveFailures > 10) {
            LOG_PARSER("CRITICAL - still no progress after ", consecutiveFailures, " attempts");
            if (!stream.isAtEnd()) {
                stream.advance();
            }
            lastPos = stream.getPos();
        }
    }
    
    // ─── Final EOF check ─────────────────────────────────────────────────
    // If we reached EOF while there were unclosed constructs, the helper
    // functions should have reported errors. This is just a safety net.
    if (stream.isAtEnd() && ctx.hasErrors) {
        // Errors already reported - nothing to do
        LOG_PARSER_DETAIL("Parse ended at EOF with errors");
    }
    
    if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        ctx.error(stream, DiagCode::E0002, MAX_CONSECUTIVE_FAILURES);
        LOG_PARSER("ERROR: Too many consecutive failures (", MAX_CONSECUTIVE_FAILURES, "), aborting");
        // No `return false` — outDecls keeps whatever was collected before
        // this threshold was hit. The caller (parse()) still builds a real
        // ModuleAST from it and flags hasErrors via ctx.hasErrors, rather
        // than discarding partial progress and signaling total failure.
        return;
    }
    
    LOG_PARSER_MINIMAL("Parsed ", declCount, " internal declarations");
}

// =============================================================================
// parseDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    // ─── 1. Skip stray semicolons ─────────────────────────────────────────
    // We import consumeTrailing here to skip all consecutive semicolons
    // at the start of a declaration (empty statements)
    stream.consumeTrailing(TokenType::SEMICOLON);
    
    if (stream.isAtEnd()) {
        return nullptr;
    }

    auto doc = harvestDocComment(stream, ctx);
    
    // ─── 2. Parse attributes ──────────────────────────────────────────────
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    // ─── 3. Parse declaration ─────────────────────────────────────────────
    DeclAST* decl = nullptr;
    
    if (stream.check(TokenType::IMPORT)) {
        // Prevent import in function body
        if (ctx.currentContext() == SyntacticContext::FuncBody) {
            return nullptr;
        }
        decl = parseImportDecl(stream, ctx);  // consumes ';' (required)
    } else if (stream.check(TokenType::STRUCT)) {
        decl = parseStructDecl(stream, ctx);
        if (decl) stream.consumeTrailing(TokenType::SEMICOLON);  // optional ';'s
    } else if (stream.check(TokenType::ENUM)) {
        decl = parseEnumDecl(stream, ctx);
        if (decl) stream.consumeTrailing(TokenType::SEMICOLON);  // optional ';'s
    } else if (stream.check(TokenType::TRAIT)) {
        decl = parseTraitDecl(stream, ctx);
        if (decl) stream.consumeTrailing(TokenType::SEMICOLON);  // optional ';'s
    } else if (stream.check(TokenType::LET) || stream.check(TokenType::CONST)) {
        if (looksLikeFuncDecl(stream, ctx)) {
            decl = parseFuncDecl(stream, ctx);
            if (decl) stream.consumeTrailing(TokenType::SEMICOLON);  // optional ';'s
        } else {
            decl = parseVarDecl(stream, ctx);  // consumes ';' (required)
        }
    } else {
        ctx.error(stream, DiagCode::E1008, stream.peekValue());
        // Same reasoning as parseInternal's recovery call: parseDecl is
        // only ever invoked at TopLevel (or, via the same dispatch, at the
        // start of a body's declaration list), and nothing has been opened
        // yet at this point, so synchronizeToContext lands on the correct
        // follow-set for wherever we actually are instead of a fixed one.
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // ─── 4. Attach attributes ──────────────────────────────────────────────
    if (decl) {
        decl->attributes = attrs;

        // Attach doc comment to the declaration
        if (doc.has_value()) {
            decl->doc = doc;
        }
    }
    
    return decl;
}

} // namespace parser