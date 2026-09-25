/// @file Parser.cpp
/// @brief The parser's two entry points: parseOneFile and parseInternal.
/// 
/// ─── What this file does ──────────────────────────────────────────────────
/// Two functions:
/// 
///   - parseOneFile:     lex, parse, and return one ModuleAST.
///   - parseInternal:    parse the top-level declarations of one file
///                       into a caller-supplied vector.
/// 
/// Everything else the parser does lives in the sibling .cpp files, grouped
/// by role (ParseDecl.cpp, ParseStmt.cpp, ParseExpr.cpp, ParseType.cpp,
/// Helpers.cpp, LookAhead.cpp). This file is the entry point; the other
/// files are the implementation of the pieces that parseOneFile calls.
/// 
/// ─── Design: one file per call ────────────────────────────────────────────
/// The parser does not walk imports. parseOneFile lexes one file, parses
/// its declarations, builds a ModuleAST, and returns. It does not read other
/// files, does not recurse, and does not know whether the file it is
/// parsing is the program's entry point or a transitively imported module.
/// 
/// The CLI is responsible for walking imports and calling parseOneFile once
/// per file. See Parser.hpp's top-of-file comment for the full design.
/// 
/// ─── Design: the context stack is empty at file boundaries ────────────────
/// A ParserContext is created once per compilation session and passed to
/// every parseOneFile call. The syntactic-context stack on the context is
/// empty before parseOneFile runs and empty after it returns. Two debug
/// assertions in parseOneFile enforce this. If either fires, the parser has
/// a bug (an unbalanced pushContext/popContext), not the input.
/// 
/// ─── Design: diagnostics carry the file ───────────────────────────────────
/// The DiagnosticEngine is shared across the whole session, so every
/// diagnostic has to be tagged with the file it came from. A
/// ScopedDiagnosticFile guard sets the engine's current file for the
/// duration of this parseOneFile call and restores the previous value on
/// exit. See ParserContext.hpp for the guard.

#include "Parser.hpp"
#include "core/Tokens.hpp"
#include "lexer/Lexer.hpp"

#include <utility>

namespace lucid::parser {

// =============================================================================
// parseOneFile — the parser's public entry point
// =============================================================================

ModuleAST* parseOneFile(std::string_view path,
                        std::string_view source,
                        ParserContext&   ctx) {
    // ─── Tag every diagnostic raised during this parse with the file ──────
    //
    // The engine is session-scoped; the file is per-parse. The guard sets
    // the engine's current file on construction and restores the previous
    // value on destruction. It restores the *previous* value rather than
    // clearing, because a caller (the CLI, the LSP) may have set a current
    // file before calling us and may still be using it after we return.
    const InternedString filePath = ctx.pool().intern(path);
    ScopedDiagnosticFile fileTag(ctx, filePath);

    // ─── Invariant: the context stack is empty on entry ───────────────────
    //
    // Under the one-file-per-call model, the parser has no cross-file state.
    // The context stack is a per-construct stack: it is pushed on entering
    // a block, a function body, a struct body, and so on, and popped on
    // leaving. By the time parseOneFile returns, every push has been
    // matched by a pop, so the stack is empty. If it isn't empty at entry,
    // a previous parseOneFile left it unbalanced, and the invariant has
    // been violated.
    //
    // AST_ASSERT_MSG fires in every build, not just debug, because a
    // violated AST invariant means the compiler is broken, not the user's
    // program. See BaseAST.hpp for the macro's contract.
    AST_ASSERT_MSG(ctx.contextStack.empty(),
                   "ParserContext context stack must be empty at file entry");

    // ─── Build the ModuleAST before parsing ───────────────────────────────
    //
    // The module is built incrementally: its `filePath` is set here, its
    // `decls` is filled in below, and its `hasErrors` is set after parsing
    // by consulting the diagnostic engine.
    //
    // Creating the module before parsing means declaration parsers can, in
    // principle, reach it through the context if they need to. Under the
    // current design, none of them do — the parser produces a flat list of
    // declarations and the module is assembled at the end. The early
    // creation is a convention that matches the previous design's shape and
    // that a future feature (module-level annotations, say) could rely on.
    auto* module = ctx.arena().make<ModuleAST>();
    module->filePath = filePath;
    module->hasErrors = false;

    // ─── Lex ──────────────────────────────────────────────────────────────
    //
    // The lexer produces a flat token stream. It reports lexer errors
    // through the same diagnostic engine. Its output always ends in an
    // EOF_TOKEN, even on error, so parseInternal always has a well-defined
    // final token to stop on.
    std::vector<Token> tokens = lexer::tokenize(source, ctx.diag());

    // ─── Empty file ───────────────────────────────────────────────────────
    //
    // A file with no tokens (empty source) still produces a valid module.
    // The lexer emits a single EOF_TOKEN for empty input, so `tokens` is
    // never truly empty — but we check defensively, in case the lexer's
    // contract changes.
    if (tokens.empty()) {
        return module;
    }

    // ─── Parse declarations ───────────────────────────────────────────────
    //
    // parseInternal reads tokens from the stream, produces DeclAST nodes,
    // and appends them to `allDecls`. It stops at EOF or when the
    // diagnostic engine has accumulated too many errors.
    TokenStream stream(std::move(tokens));
    std::vector<DeclAST*> allDecls;
    parseInternal(stream, ctx, allDecls);

    // ─── Assemble the module ──────────────────────────────────────────────
    //
    // The declarations are moved into an arena-allocated span. The span is
    // immutable once built; the ModuleAST holds it as `decls`.
    auto declsBuilder = ctx.arena().makeBuilder<DeclAST*>(allDecls.size());
    for (DeclAST* decl : allDecls) {
        declsBuilder.push_back(decl);
    }
    module->decls = declsBuilder.build();

    // ─── Record whether the parse produced errors ─────────────────────────
    //
    // The module's `hasErrors` flag tells callers (the CLI, the LSP)
    // whether the parse produced a usable AST. The flag is a convenience;
    // the diagnostic engine is the source of truth. But a ModuleAST with
    // `hasErrors == true` and a clean engine is a bug, and a ModuleAST with
    // `hasErrors == false` and a dirty engine is also a bug — the flag
    // mirrors the engine's state at the moment the parse finished, no more.
    //
    // The flag is set to the engine's *current* state, not to "did this
    // file produce errors". If a previous file's parse left errors in the
    // engine, this file's module has `hasErrors == true` even if this file
    // is clean. In practice the CLI clears the engine between files, or
    // treats the engine's state as the session-wide answer, so the
    // per-module flag is informational.
    module->hasErrors = ctx.diag().hasErrors();

    // ─── Invariant: the context stack is empty on exit ────────────────────
    //
    // Every parse function that pushes a context frame must pop it before
    // returning. This assertion catches an unbalanced push in the parser
    // itself, at the point where the imbalance is detected.
    AST_ASSERT_MSG(ctx.contextStack.empty(),
                   "ParserContext context stack must be empty at file exit");

    return module;
}

// =============================================================================
// parseInternal — parse a file's top-level declarations
// =============================================================================

void parseInternal(TokenStream& stream,
                   ParserContext& ctx,
                   std::vector<DeclAST*>& outDecls) {
    // ─── The main loop ────────────────────────────────────────────────────
    //
    // Read declarations until the stream is exhausted or the diagnostic
    // engine says to stop. Each iteration either:
    //
    //   - consumes a stray `;` and continues (empty statements at top
    //     level are harmless and common after a declaration),
    //   - parses a declaration and appends it,
    //   - or reports an error and synchronizes to the next declaration.
    //
    // The loop can make progress on any of these paths; the only way it
    // terminates without producing a declaration is at EOF or when
    // ctx.canContinue() returns false.
    while (!stream.isAtEnd() && ctx.canContinue()) {
        // ─── Skip stray semicolons ────────────────────────────────────────
        //
        // A stray `;` at top level is a no-op. This happens after a
        // declaration that already consumed its terminating `;`, or when
        // the user wrote `;;` by accident. Skipping is silent.
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        // ─── Check for a declaration start ────────────────────────────────
        //
        // A top-level declaration starts with one of the declaration
        // keywords, or with `@` for an attribute list. Anything else is
        // either a statement keyword (illegal at top level) or a token
        // that cannot begin a declaration.
        //
        // The check produces a targeted error: a control-flow keyword at
        // top level gets "statements are not allowed at top level", a
        // concurrency keyword gets the same, and any other token gets
        // "expected a declaration".
        const TokenType current = stream.peekType();
        const bool atDeclStart =
            current == TokenType::KW_IMPORT  ||
            current == TokenType::KW_TYPE    ||
            current == TokenType::KW_STRUCT  ||
            current == TokenType::KW_ENUM    ||
            current == TokenType::KW_FN      ||
            current == TokenType::KW_CONST   ||
            current == TokenType::KW_LET     ||
            current == TokenType::KW_TRAIT   ||
            current == TokenType::KW_SATISFY ||
            current == TokenType::KW_DEF     ||
            current == TokenType::AT_SIGN;

        if (!atDeclStart) {
            // Report the appropriate error for what we found.
            if (isStatementKeyword(current) ||
                isConcurrencyKeyword(current)) {
                ctx.diag().errorAt(
                    diag::DiagCode::Syntax_UnexpectedToken,
                    stream.currentLoc(),
                    "statement keyword '", stream.peekValue(),
                    "' is not allowed at top level; expected a declaration");
            } else {
                ctx.diag().errorAt(
                    diag::DiagCode::Syntax_UnexpectedToken,
                    stream.currentLoc(),
                    "expected a declaration, got '", stream.peekValue(), "'");
            }

            // Synchronize to the next plausible declaration start.
            // `synchronizeToBoundary` stops at declaration keywords and at
            // statement keywords, which is the correct follow-set for a
            // top-level recovery: the next declaration starts at a
            // declaration keyword, and a statement keyword that
            // accidentally appears at top level is itself an error to be
            // reported by the next iteration.
            synchronizeToBoundary(stream, ctx,
                                  {TokenType::AT_SIGN, TokenType::SEMICOLON});

            if (stream.isAtEnd()) break;
            continue;
        }

        // ─── Parse the declaration ────────────────────────────────────────
        //
        // parseDecl dispatches on the current keyword and returns a
        // DeclAST. It reports its own errors; a declaration that recovered
        // from a syntax error is marked `hasSyntaxError` but is still
        // returned, so the parser can continue.
        //
        // A nullptr return from parseDecl means a caller bug — the
        // dispatcher only returns null when it was called with a token
        // that isn't a declaration start, which we just checked. We treat
        // it defensively: if parseDecl returns null, synchronize and
        // continue rather than crashing.
        const size_t posBefore = stream.getPos();

        DeclAST* decl = parseDecl(stream, ctx);

        if (decl != nullptr) {
            outDecls.push_back(decl);
            continue;
        }

        // ─── parseDecl returned null ──────────────────────────────────────
        //
        // This should not happen for a well-formed dispatch, but it can if
        // a parser bug causes parseDecl to fail to produce a node. If the
        // stream has not advanced, we would loop forever; synchronize to
        // make progress.
        if (stream.getPos() == posBefore) {
            ctx.diag().errorAt(
                diag::DiagCode::Syntax_IncompleteDeclaration,
                stream.currentLoc(),
                "parser could not recover from the previous error");

            synchronizeToBoundary(stream, ctx,
                                  {TokenType::AT_SIGN, TokenType::SEMICOLON});

            if (stream.isAtEnd()) break;
        }
        // If the stream advanced but parseDecl returned null, the next
        // iteration will handle whatever token we're on. Loop.
    }
}

} // namespace lucid::parser