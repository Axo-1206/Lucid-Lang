/**
 * @file Parser.cpp
 * @brief The parser's single entry point: parseFile.
 *
 * ─── What this file does ──────────────────────────────────────────────────
 * One function: parseFile. It lexes the source, builds a TokenStream, runs
 * the top-level declaration loop, assembles the ModuleAST, and returns.
 *
 * Everything else the parser does lives in the sibling .cpp files, grouped
 * by role:
 *
 *   - ParseDecl.cpp    declarations and their sub-parts
 *   - ParseStmt.cpp    statements and control flow
 *   - ParseExpr.cpp    expressions
 *   - ParseType.cpp    types
 *   - ParseAttr.cpp    attributes
 *   - Helpers.cpp      shared list parsers and utility functions
 *
 * ─── Design: one file per call ────────────────────────────────────────────
 * The parser does not walk imports. parseFile lexes one file, parses its
 * declarations, builds a ModuleAST, and returns. It does not read other
 * files, does not recurse, and does not know whether the file it is
 * parsing is the program's entry point or a transitively imported module.
 *
 * The CLI is responsible for walking imports and calling parseFile once
 * per file. See Parser.hpp's top-of-file comment for the full design.
 *
 * ─── Design: the top-level loop is here, not in a helper ──────────────────
 * The loop that reads top-level declarations had its own function in the
 * previous design (parseInternal). It has exactly one caller, and that
 * caller is parseFile. Extracting it would add a parameter list and a
 * forward declaration for no benefit. It lives inline in parseFile.
 *
 * ─── Design: diagnostics carry the file ───────────────────────────────────
 * The DiagnosticEngine is shared across the whole session, so every
 * diagnostic has to be tagged with the file it came from. A
 * ScopedDiagnosticFile guard sets the engine's current file for the
 * duration of this parseFile call and restores the previous value on exit.
 */

#include "Parser.hpp"
#include "parser/support/ErrorRecovery.hpp"
#include "parser/support/GrammarPositions.hpp"

#include "core/Tokens.hpp"
#include "core/ast/DeclAST.hpp"
#include "lexer/Lexer.hpp"

#include <optional>
#include <utility>
#include <vector>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseFile — the parser's single entry point
// =============================================================================

ModuleAST* parseFile(std::string_view path,
                     std::string_view source,
                     ParserContext&   ctx) {
    // ─── Tag every diagnostic raised during this parse with the file ──────
    //
    // The engine is session-scoped; the file is per-parse. The guard sets
    // the engine's current file on construction and restores the previous
    // value on destruction. It restores the *previous* value rather than
    // clearing, because a caller (the CLI, the LSP) may have set a current
    // file before calling us and may still be using it after we return.
    const InternedString filePath = ctx.pool.intern(path);
    ScopedDiagnosticFile fileTag(ctx, filePath);

    // ─── Build the ModuleAST up front ─────────────────────────────────────
    //
    // The module is built incrementally: its filePath is set here, its
    // decls is filled in below, and its hasErrors is set after parsing by
    // consulting the diagnostic engine.
    //
    // Creating the module before parsing means the return value is never
    // null. A file that fails to lex entirely still produces a real
    // ModuleAST with no declarations and hasErrors == true.
    auto* module = ctx.arena.make<ModuleAST>();
    module->filePath = filePath;
    module->hasErrors = false;

    // ─── Lex ──────────────────────────────────────────────────────────────
    //
    // The lexer produces a flat token stream, interning every token payload
    // through the pool. It reports lexer errors through the same diagnostic
    // engine. Its output always ends in an EOF_TOKEN, even on error, so the
    // parse loop always has a well-defined final token.
    std::vector<Token> tokens = lexer::tokenize(source, ctx.pool, ctx.diag);

    // Defensive: the lexer's contract says the vector is never empty (it
    // always appends an EOF token). If that contract ever changes, this
    // early return keeps parseFile from handing a degenerate stream to the
    // loop below.
    if (tokens.empty()) {
        return module;
    }

    // ─── Parse ────────────────────────────────────────────────────────────
    TokenStream stream(std::move(tokens));

    std::vector<DeclAST*> allDecls;

    // The stop set used by both the dispatch check and the recovery scan.
    // The two must agree: the dispatch recognizes exactly the tokens the
    // recovery targets, so a declaration that the dispatch accepted is
    // always reachable after a recovery, and a declaration that recovery
    // lands on is always recognized by the next dispatch.
    const auto isDeclStart = [](TokenType t) {
        return isDeclarationStart(t) || t == TokenType::SEMICOLON;
    };

    // The top-level loop. Read declarations until the stream is exhausted
    // or the diagnostic engine says to stop. Each iteration either:
    //
    //   - consumes a stray `;` and continues,
    //   - parses a declaration and appends it,
    //   - or reports an error and synchronizes to the next declaration.
    //
    // The loop can make progress on any of these paths. The only way it
    // exits without a declaration is at EOF or when ctx.canContinue()
    // returns false.
    while (!stream.isAtEnd() && ctx.canContinue()) {
        // ─── Skip stray semicolons ────────────────────────────────────────
        //
        // A stray `;` at top level is a no-op. The grammar has no top-level
        // statements, but a `;` after a declaration that already consumed
        // its own terminator is common, and reporting it would be noise.
        if (stream.match(TokenType::SEMICOLON)) {
            continue;
        }

        // ─── Check for a declaration start ────────────────────────────────
        //
        // A top-level declaration begins with one of the declaration
        // keywords, with `import`, or with `@` for an attribute list. 
        // isDeclarationStart (GrammarPositions.hpp) is the
        // source of truth for that set.
        if (!isDeclarationStart(stream.peekType())) {
            ctx.diag.errorAt(
                DiagCode::Syntax_UnexpectedToken,
                stream.currentLoc(),
                "expected a declaration, got '",
                stream.peekValueView(ctx.pool), "'");

            // Synchronize to the next plausible declaration start.
            // The scan is depth-aware: a strong declaration start at
            // depth > 0 means an enclosing block lost its `}` and the
            // scan should stop there so the caller can resume at the
            // declaration.
            synchronizeUntilDepth(stream, isTopLevelRecoveryStop);

            if (stream.isAtEnd()) break;
            continue;
        }

        // ─── Harvest the doc comment ──────────────────────────────────────
        //
        // The doc comment is a property of the declaration that follows it,
        // not of the declaration parser. harvestDocComment scans backward
        // from the current cursor, through the raw token vector (which
        // includes DOC_COMMENT tokens the cursor skips), to find a comment
        // attached to this declaration.
        //
        // The harvest must happen before parseDecl, because parseDecl
        // consumes the tokens the harvester scans past.
        std::optional<DocComment> doc = harvestDocComment(stream, ctx);

        // ─── Parse the declaration ────────────────────────────────────────
        //
        // parseDecl reads any attribute sequence, dispatches on the
        // declaration keyword, and attaches the attributes to the returned
        // node. It reports its own errors; a declaration that recovered
        // from a syntax error is marked hasSyntaxError but is still
        // returned.
        //
        // parseDecl never returns nullptr: it wraps a nullptr from a
        // specific parser in a marked UnknownDeclAST. The check below is
        // defensive against a future change to that contract.
        const size_t posBefore = stream.getPos();

        DeclAST* decl = parseDecl(stream, ctx);

        if (decl != nullptr) {
            // Attach the doc comment. parseDecl does not touch doc
            // comments; they are the caller's responsibility, because only
            // the caller knows where in the file the declaration begins.
            if (doc.has_value()) {
                decl->doc = doc;
            }
            allDecls.push_back(decl);
            continue;
        }

        // ─── parseDecl returned nullptr ───────────────────────────────────
        //
        // This branch should be unreachable under the current parseDecl
        // contract, but if it fires, we must make progress. If the stream
        // did not advance, synchronize to avoid an infinite loop. If it
        // did advance, the next iteration will handle whatever token we
        // are on.
        if (stream.getPos() == posBefore) {
            ctx.diag.errorAt(
                DiagCode::Syntax_IncompleteDeclaration,
                stream.currentLoc(),
                "parser could not recover from the previous error");

            synchronizeUntilDepth(stream, isTopLevelRecoveryStop);

            if (stream.isAtEnd()) break;
        }
    }

    // ─── Assemble the module ──────────────────────────────────────────────
    //
    // The declarations are moved into an arena-allocated span. The span is
    // immutable once built; the ModuleAST holds it as `decls`.
    if (!allDecls.empty()) {
        auto declsBuilder = ctx.arena.makeBuilder<DeclAST*>(allDecls.size());
        for (DeclAST* decl : allDecls) {
            declsBuilder.push_back(decl);
        }
        module->decls = declsBuilder.build();
    }

    // ─── Record whether the parse produced errors ─────────────────────────
    //
    // The flag is a convenience; the diagnostic engine is the source of
    // truth. It mirrors the engine's state at the moment the parse
    // finished, which is session-scoped: if a previous file's parse left
    // errors in the engine, this file's module has hasErrors == true even
    // if this file is clean. Callers that want the per-file answer check
    // the diagnostics raised between the start and end of this call.
    module->hasErrors = ctx.diag.hasErrors();

    return module;
}

} // namespace lucid::parser