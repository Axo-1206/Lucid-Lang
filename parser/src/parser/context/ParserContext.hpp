/// @file ParserContext.hpp
///
/// @brief Per-session parser state: the compilation session handle.
///
/// ─── What this is ─────────────────────────────────────────────────────────
/// The parser is called once per source file and produces one ModuleAST.
/// It does not walk imports, does not resolve module paths, and does not
/// depend on the filesystem. Those are the CLI's jobs; see the architecture
/// document's pipeline diagram, where ModuleResolver precedes Parsing in
/// the CLI's column.
///
/// Because the parser does not recurse across files, this context is small.
/// It holds references to the string pool, the AST arena, and the
/// diagnostic engine. The parser does not own any of them.
///
/// ─── What changed from the previous design ────────────────────────────────
/// The previous design carried a syntactic-context stack: a std::vector of
/// frames recording where in the grammar the parser was (top level, inside
/// a function body, inside a struct body, ...). Error recovery consulted
/// the top of the stack to pick a follow-set.
///
/// The new grammar does not need it. There are no structs, no enums, no
/// traits, no DEF — the enum's variants named constructs that do not exist
/// in the language. And the new parser does not do context-dependent
/// recovery: it recovers by shape (the next declaration keyword, the next
/// statement keyword, a closing brace) rather than by consulting a stack of
/// pushed frames. Removing the stack removes the machinery whose only job
/// was to keep itself consistent — the two AST_ASSERT_MSG calls that
/// checked "the stack is empty at file entry / exit" existed to catch a
/// push without a pop, and there are no pushes left.
///
/// ─── What this is NOT ─────────────────────────────────────────────────────
/// It is not a session. The resources it references are owned by the
/// session (or by whatever constructed the context), and outlive the
/// parse. A ParserContext borrows them; it does not extend their lifetime.
///
/// It is not per-file in the sense of "one context per file". One
/// ParserContext is constructed per session, and every file parsed by that
/// session uses the same one.
///
/// It is not a place to stash data that some other pass wants. A field
/// that is written by the parser and read by Sema does not belong here;
/// it belongs on the AST node the parser produced.

#pragma once

#include "core/SourceLocation.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

namespace lucid::parser {

/// @brief The parser's view of the compilation session.
///
/// Constructed once per session and passed by reference to every parser
/// function. Holds references to the resources the parser needs; the
/// parser reaches them as plain fields.
struct ParserContext {
    /// Canonical string storage for the session.
    StringPool& pool;

    /// Bump-allocated storage for the session's AST.
    ASTArena& arena;

    /// Collected diagnostics for the session.
    lucid::diag::DiagnosticEngine& diag;

    // ─── Construction ──────────────────────────────────────────────────

    ParserContext(StringPool& p,
                  ASTArena& a,
                  lucid::diag::DiagnosticEngine& d)
        : pool(p)
        , arena(a)
        , diag(d)
    {}

    // Non-copyable, non-movable: the parser holds a reference to it, and
    // the reference must remain valid for the whole parse. Making this
    // type movable would allow a copy to be made and then destroyed,
    // leaving the parser with a dangling reference.
    ParserContext(const ParserContext&)            = delete;
    ParserContext& operator=(const ParserContext&) = delete;
    ParserContext(ParserContext&&)                 = delete;
    ParserContext& operator=(ParserContext&&)      = delete;

    /// True if the parser should keep going. The error cap is the
    /// diagnostic engine's; this wrapper exists so call sites read
    /// `ctx.canContinue()` rather than reaching through to the engine.
    bool canContinue() const {
        return diag.canContinue();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ScopedDiagnosticFile
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Tags every diagnostic raised while active with a file identity.
///
/// The diagnostic engine is one per session, but a session parses many
/// files. A diagnostic raised during a parse has to carry the file it came
/// from, or a session that parses three files produces three files' worth
/// of "line 12, column 5" with no way to tell them apart.
///
/// This guard sets the engine's current file on construction and restores
/// the previous value on destruction. The "previous" matters: an LSP
/// analysis runs on a background file while the user is looking at another,
/// and the engine's current file has to return to whatever it was after
/// the analysis completes.
struct ScopedDiagnosticFile {
    ScopedDiagnosticFile(ParserContext& ctx, InternedString file)
        : ctx_(ctx)
        , saved_(ctx_.diag.currentFile())
    {
        ctx_.diag.setCurrentFile(file);
    }

    ~ScopedDiagnosticFile() {
        ctx_.diag.setCurrentFile(saved_);
    }

    ScopedDiagnosticFile(const ScopedDiagnosticFile&)            = delete;
    ScopedDiagnosticFile& operator=(const ScopedDiagnosticFile&) = delete;
    ScopedDiagnosticFile(ScopedDiagnosticFile&&)                 = delete;
    ScopedDiagnosticFile& operator=(ScopedDiagnosticFile&&)      = delete;

private:
    ParserContext& ctx_;
    InternedString saved_;
};

} // namespace lucid::parser