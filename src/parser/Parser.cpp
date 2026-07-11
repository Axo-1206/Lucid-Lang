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
 * - parseUseDecl(): Parses use declaration and imports the module
 */

#include "Parser.hpp"
#include "lexer/Lexer.hpp"
#include "core/ast/BaseAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

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
 */
template<typename Predicate>
void synchronizeUntil(TokenStream& stream, ParserContext& ctx, Predicate stopAt) {
    LOG_PARSER("Synchronizing");
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
            // belongs to an enclosing construct. Stop, don't consume.
            LOG_PARSER_DETAIL("Synchronization stopped before enclosing closer: ",
                               debug::tokenTypeToString(current));
            return;
        }

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
 */
void synchronizeToContext(TokenStream& stream, ParserContext& ctx) {
    switch (ctx.currentContext()) {
        case SyntacticContext::Attribute:
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RBRACKET);
            return;

        case SyntacticContext::GenericParams:
        case SyntacticContext::GenericArgs:
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::GREATER);
            return;

        case SyntacticContext::FuncParams:
            synchronizeTo(stream, ctx, TokenType::COMMA, TokenType::RPAREN);
            return;

        case SyntacticContext::FuncBody:
        case SyntacticContext::StructBody:
        case SyntacticContext::EnumBody:
        case SyntacticContext::TraitBody:
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::SEMICOLON
                    || t == TokenType::RBRACE
                    || is_declaration_keyword(t);
            });
            return;

        case SyntacticContext::TopLevel:
        default:
            synchronizeUntil(stream, ctx, [](TokenType t) {
                return t == TokenType::SEMICOLON || is_declaration_keyword(t);
            });
            return;
    }
}

// =============================================================================
// parse() - ENTRY POINT
// =============================================================================

/**
 * @brief Parse a single source file into a ModuleAST, recursively resolving
 * its `use` imports as needed.
 *
 * This is the ONE and ONLY entry point for parsing — the driver calls it
 * once for main.luc, and it is re-entered recursively (via parseUseDecl)
 * once per not-yet-parsed imported file.
 *
 * ## Model: modules, not a flat merge
 *
 * Each call returns a ModuleAST containing only THIS file's own top-level
 * declarations. Imported files are NOT inlined/flattened into it — a
 * `use path [as alias]` becomes a UseDeclAST that stores a reference
 * (path + alias) to another module. That referenced module is resolved to
 * its own ModuleAST separately, via ModuleResolver's cache, and symbol
 * lookup across the reference (e.g. `math.sqrt`) is the semantic/binder
 * pass's job, not parse()'s. This preserves per-file namespacing — two
 * modules may each declare a `Point` without colliding — which a flat,
 * single-Program merge would not.
 *
 * The "whole program" is therefore not one object parse() builds and
 * returns. It's the graph formed by the root ModuleAST plus every module
 * transitively reachable from it through ModuleResolver's cache — walked
 * by later compiler stages, not assembled here.
 *
 * ## Flow
 *
 * ```
 * 1. Driver calls parse(main.luc, source, ctx)
 *       → the one true entry point; re-entered recursively for every `use`
 *
 * 2. parse(path, source, ctx):
 *    a. Intern path, ctx.currentFilePath = path
 *    b. ScopedFileContext   — save this file's caller's contextStack and
 *                             error-tracking fields, reset both for this
 *                             file (this file starts at TopLevel with no
 *                             errors yet), restore the caller's on return
 *                             (draining this file's own errors into
 *                             ctx.allDiagnostics first)
 *    c. Push filePath onto resolver's "in progress" stack (ScopedParsingGuard);
 *       if it's already on that stack → circular import, reject
 *    d. Lex source → tokens; construct TokenStream for this file
 *    e. Loop: parseDecl() until EOF                    ─┐
 *         │                                              │ (step 3, expanded)
 *         └── every declaration goes into                │
 *             this file's own decl list                  │
 *    f. Build ModuleAST from that decl list
 *    g. resolver->cacheModule(path, thisModule)   — memoize, so a second
 *                                                    `use` of this file is
 *                                                    a cache hit, not a
 *                                                    re-parse
 *    h. Pop this file off the "in progress" stack (ScopedParsingGuard),
 *       restore the caller's contextStack/errors (ScopedFileContext)
 *    i. return ModuleAST*
 *
 * 3. Inside the loop (2e), whenever parseDecl() hits a
 *    `use path [as alias]`:
 *    a. Resolve `path` → file path
 *    b. resolver->getParsedModule(filePath):
 *         cache HIT  → reuse existing ModuleAST, done, no recursion
 *         cache MISS → GOTO STEP 2 (recursive call:
 *                       parse(filePath, source, ctx))
 *                       → returns the child ModuleAST, already cached
 *                         by that call's own step 2g (not repeated here —
 *                         a second cacheModule() call here would bypass
 *                         parse()'s own !ctx.hasErrors guard)
 *    c. UseDeclAST stores {path, alias} — a REFERENCE to the module,
 *       not its inlined content.
 *    d. Control returns to 3, continues the CURRENT file's loop at 2e
 *
 * 4. Root parse() call returns the root ModuleAST.
 *    The full "program" = root ModuleAST + resolver's cache of every
 *    module reachable from it — a dependency graph, not a flat list.
 * ```
 *
 * @param path The file path
 * @param source The source code
 * @param ctx The parsing context (shared across all files)
 * @return ModuleAST* This file's own declarations, or nullptr on error.
 *         Does NOT include imported files' declarations — see them via
 *         their own ModuleAST, reachable through ctx.resolver.
 */
ModuleAST* parse(const std::string& path, 
                  const std::string& source,
                  ParserContext& ctx) {
    LOG_PARSER_MINIMAL("Parsing file: ", path);
    
    // ─── 1. Intern the file path ────────────────────────────────────────
    InternedString filePath = ctx.pool.intern(path);
    ctx.currentFilePath = filePath;

    // Save the importing file's contextStack and error-tracking fields
    // (if any), reset both for this file, and restore them on return —
    // on every exit path below, including the early returns. This also
    // owns the clearErrors() reset directly; see ScopedFileContext's doc
    // comment for why that can't safely be a separate call site anymore.
    ScopedFileContext fileContext(ctx);
    
    // ─── 2. Check for Cyclic Dependencies ───────────────────────────────
    // We put the file that currently parsing into a stack, if the next
    // file is already in the stack then it's a Cyclic Dependencies
    if (ctx.resolver && ctx.resolver->isParsing(filePath)) {
        LOG_PARSER("ERROR: Circular import detected: ", path);
        return nullptr;
    }
    // Push filePath onto the parsing stack for cycle detection; popped
    // automatically on every return path below, including the lex-failure
    // and parseInternal-failure early returns further down.
    ScopedParsingGuard parsingGuard(ctx.resolver, filePath);
    
    // ─── 3. Lex the Source ──────────────────────────────────────────────
    auto tokens = lexer::tokenize(source, path);
    if (tokens.empty()) {
        LOG_PARSER_MINIMAL("Lexer produced no tokens for: ", path);
        return nullptr;
    }
    
    // Check for lexer errors
    for (const auto& tok : tokens) {
        if (tok.type == TokenType::UNKNOWN) {
            LOG_PARSER_MINIMAL("Lexer error in: ", path);
            return nullptr;
        }
    }
    
    // ─── 4. Create TokenStream ──────────────────────────────────────────
    TokenStream stream(std::move(tokens), filePath);
    
    // ─── 5. Parse Internal Declarations ─────────────────────────────────
    std::vector<DeclPtr> allDecls;
    
    if (!parseInternal(stream, ctx, allDecls)) {
        return nullptr;
    }
    
    // ─── 6. Build this file's ModuleAST ──────────────────────────────────
    auto* thisModule = ctx.arena.make<ModuleAST>();
    thisModule->filePath = filePath;
    
    auto builder = ctx.arena.makeBuilder<DeclPtr>();
    for (auto* d : allDecls) {
        builder.push_back(d);
    }
    thisModule->decls = builder.build();
    
    // ─── 7. Cache the result ─────────────────────────────────────────────
    // (parsingGuard pops this file off the parsing stack automatically
    // when parse() returns, below — no manual popParsing() needed here.)
    if (ctx.resolver && !ctx.hasErrors) {
        ctx.resolver->cacheModule(filePath, thisModule);
        LOG_PARSER_DETAIL("Cached module: ", path);
    }
    
    LOG_PARSER_MINIMAL("Parse completed: ", allDecls.size(), " total declarations");
    
    return thisModule;
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
 *     │       │       ├── USE → parseUseDecl()
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
 *     │   ├── if (consecutiveFailures >= MAX)
 *     │   │   ├── ctx.error(stream, DiagCode::E0002, MAX)
 *     │   │   └── return false
 *     │   │
 *     │   └── if (stream.isAtEnd() && ctx.hasErrors)
 *     │       └── Log that errors occurred (safety net)
 *     │
 *     └── 4. Return
 *         ├── LOG_PARSER_MINIMAL("Parsed N declarations")
 *         └── return true
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
 * - Imported modules are parsed recursively via `parseUseDecl()`
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
 * 
 * @return true if parsing succeeded (including with recoverable errors).
 *         false if a fatal error occurred and parsing was aborted.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * std::vector<DeclPtr> decls;
 * if (!parseInternal(stream, ctx, decls)) {
 *     // Fatal error - cannot continue
 *     return nullptr;
 * }
 * // All declarations are in decls
 * ```
 * 
 * @note This function does NOT handle imports directly. Imports are handled
 *       by parseUseDecl() which is called from parseDecl().
 *       The imported module's declarations are collected recursively
 *       when parseUseDecl() calls parse() on the imported file.
 * 
 * @warning If this function returns false, the token stream may be in an
 *          inconsistent state. The caller should not attempt to continue
 *          parsing the same file.
 */
bool parseInternal(TokenStream& stream, ParserContext& ctx, std::vector<DeclPtr>& outDecls) {
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
        return false;
    }
    
    LOG_PARSER_MINIMAL("Parsed ", declCount, " internal declarations");
    return true;
}

// =============================================================================
// parseDecl() - Dispatch to specific declaration parsers
// =============================================================================

DeclAST* parseDecl(TokenStream& stream, ParserContext& ctx) {
    // ─── 1. Skip stray semicolons ─────────────────────────────────────────
    // We use consumeTrailing here to skip all consecutive semicolons
    // at the start of a declaration (empty statements)
    stream.consumeTrailing(TokenType::SEMICOLON);
    
    if (stream.isAtEnd()) {
        return nullptr;
    }
    
    // ─── 2. Parse attributes ──────────────────────────────────────────────
    ArenaSpan<AttributePtr> attrs = parseAttributes(stream, ctx);
    
    // ─── 3. Parse declaration ─────────────────────────────────────────────
    DeclAST* decl = nullptr;
    
    if (stream.check(TokenType::USE)) {
        decl = parseUseDecl(stream, ctx);  // consumes ';' (required)
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
    }
    
    return decl;
}

} // namespace parser