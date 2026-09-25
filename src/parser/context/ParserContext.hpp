/// @file ParserContext.hpp
/// @brief Per-session parser state: the compilation session handle and the
///        syntactic-context stack.
///
/// ─── What this is ─────────────────────────────────────────────────────────
/// The parser is called once per source file and produces one ModuleAST.
/// It does not walk imports, does not resolve module paths, and does not
/// depend on the filesystem. Those are the CLI's jobs; see the architecture
/// document's pipeline diagram, where "ModuleResolver" precedes "Parsing"
/// in the CLI's column.
///
/// Because the parser no longer recurses across files, this context is much
/// smaller than the previous design's. It holds:
///
///   1. A reference to the session, which owns the string pool, the AST
///      arena, and the diagnostic engine. The parser reaches all three
///      through the session; it does not own them.
///
///   2. The syntactic-context stack, which records where in the grammar the
///      parser currently is (top level, inside a function body, inside a
///      struct body, ...). Error recovery consults it to pick a follow-set.
///
/// That is the whole of it. There is no module resolver, no "current module"
/// pointer, and no cross-file state.
///
/// ─── What this is NOT ─────────────────────────────────────────────────────
/// It is not a session. A CompilationSession owns the pool, the diagnostic
/// engine, and the arena, and lives for the duration of a whole compilation.
/// A ParserContext borrows a session and adds the parser's own state on top.
///
/// It is not per-file in the sense of "one context per file". One
/// ParserContext is constructed per session, and every file parsed by that
/// session uses the same one. The context stack is asserted empty at the
/// start and end of each file's parse; see parseOneFile in Parser.cpp.
///
/// It is not a place to stash data that some other pass wants. A field
/// that is written by the parser and read by Sema does not belong here; it
/// belongs on the AST node the parser produced.

#pragma once

#include "core/CompilationSession.hpp"
#include "core/SourceLocation.hpp"
#include "core/ast/BaseAST.hpp"

#include <vector>

namespace lucid::parser {

// ─────────────────────────────────────────────────────────────────────────────
// SyntacticContext
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The kind of construct the parser is currently inside.
///
/// Pushed when the parser enters a construct and popped when it leaves.
/// Error recovery reads the top of the stack to decide which tokens can
/// safely end the current production — for example, a top-level declaration
/// can be terminated by any declaration keyword, but a `switch` case can
/// only be terminated by `case`, `default`, or `}`.
///
/// The set reflects the frames the grammar actually has. Adding a frame is
/// a grammar change; every frame here is one the parser pushes somewhere.
enum class SyntacticContext {
    TopLevel,       // File-level declarations
    Attribute,      // @[ ... ]
    GenericParams,  // < ... > (declaration site: struct<T>, fn<T>)
    GenericArgs,    // < ... > (use site: Map<int, string>)
    FuncParams,     // ( ... ) parameter list
    FuncBody,       // { ... } function body
    FieldBody,      // function-typed struct field's block default
    StructBody,     // struct { ... } — a `TYPE X = struct` target
    EnumBody,       // enum { ... } — a `TYPE X = enum` target
    TraitBody,      // trait { ... }
    DefBody,        // DEF ... = { ... } — the DEF's block implementation
    SwitchBody,     // switch { ... } — special recovery: stop at case/default/RBRACE
};

inline const char* syntacticContextName(SyntacticContext kind) noexcept {
    switch (kind) {
        case SyntacticContext::TopLevel:      return "top level";
        case SyntacticContext::Attribute:     return "attribute list";
        case SyntacticContext::GenericParams: return "generic parameter list";
        case SyntacticContext::GenericArgs:   return "generic argument list";
        case SyntacticContext::FuncParams:    return "function parameter list";
        case SyntacticContext::FuncBody:      return "function body";
        case SyntacticContext::FieldBody:     return "field body";
        case SyntacticContext::StructBody:    return "struct body";
        case SyntacticContext::EnumBody:      return "enum body";
        case SyntacticContext::TraitBody:     return "trait body";
        case SyntacticContext::DefBody:       return "DEF body";
        case SyntacticContext::SwitchBody:    return "switch body";
    }
    return "unknown context";
}

/// @brief One frame on the context stack.
struct ContextFrame {
    SyntacticContext kind;
    SourceLocation   openedAt;
};

// ─────────────────────────────────────────────────────────────────────────────
// ParserContext
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The parser's view of the compilation session, plus its own state.
///
/// Constructed once per session and passed by reference to every parser
/// function. See the file's top-of-file comment for the design rationale.
struct ParserContext {
    // ─── The session ───────────────────────────────────────────────────
    //
    // Owns the pool, the diagnostic engine, and the arena. The parser
    // reaches them through the accessors below; it does not hold them
    // directly, so there is no chance of the parser's view of the session
    // drifting from the session itself.
    CompilationSession& session;

    // ─── The syntactic-context stack ───────────────────────────────────
    //
    // Pushed on entering a construct and popped on leaving. The stack is
    // asserted empty at the start and end of each parseOneFile call, so
    // an unbalanced push/pop is caught immediately rather than leaking
    // into the next file.
    std::vector<ContextFrame> contextStack;

    // ─── Construction ──────────────────────────────────────────────────

    explicit ParserContext(CompilationSession& s) : session(s) {}

    // Non-copyable, non-movable: the parser holds a reference to it, and
    // the reference must remain valid for the whole parse. Making this
    // type movable would allow a copy to be made and then destroyed,
    // leaving the parser with a dangling reference.
    ParserContext(const ParserContext&)            = delete;
    ParserContext& operator=(const ParserContext&) = delete;
    ParserContext(ParserContext&&)                 = delete;
    ParserContext& operator=(ParserContext&&)      = delete;

    // ─── Session accessors ─────────────────────────────────────────────
    //
    // The three names every parser function reaches for. They forward to
    // the session; there is no duplicate storage.

    StringPool&             pool()  noexcept { return session.pool; }
    ASTArena&               arena() noexcept { return session.arena; }
    lucid::diag::DiagnosticEngine& diag() noexcept { return session.diagnostics; }

    /// True if the parser should keep going. The threshold is the diagnostic
    /// engine's error cap; past it, every construct produces an error and
    /// the recovery paths cascade. Stopping early gives a cleaner report.
    bool canContinue(int maxErrors = 100) const {
        return session.diagnostics.canContinue(maxErrors);
    }

    // ─── Context-stack operations ──────────────────────────────────────

    void pushContext(SyntacticContext kind, const SourceLocation& loc) {
        contextStack.push_back(ContextFrame{kind, loc});
    }

    void popContext() {
        if (!contextStack.empty()) {
            contextStack.pop_back();
        }
    }

    /// The innermost open construct. A fresh context is at TopLevel.
    SyntacticContext currentContext() const noexcept {
        return contextStack.empty() ? SyntacticContext::TopLevel
                                    : contextStack.back().kind;
    }

    /// True if any frame on the stack is `kind`.
    bool isInsideContext(SyntacticContext kind) const noexcept {
        for (const auto& frame : contextStack) {
            if (frame.kind == kind) return true;
        }
        return false;
    }

    /// The source location where the innermost construct opened. Used by
    /// diagnostics that want to say "the unclosed '{' opened here".
    SourceLocation currentContextOpenedAt() const noexcept {
        return contextStack.empty() ? SourceLocation{}
                                    : contextStack.back().openedAt;
    }

    size_t contextDepth() const noexcept { return contextStack.size(); }

    /// True when the parser is at the top level of a file.
    bool isTopLevel() const noexcept {
        return currentContext() == SyntacticContext::TopLevel;
    }

    /// True when the parser is inside a function body. A function-typed
    /// field's block default is a body too, so it counts.
    bool isInsideFuncBody() const noexcept {
        return isInsideContext(SyntacticContext::FuncBody)
            || isInsideContext(SyntacticContext::FieldBody);
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
///
/// This is the only RAII guard the parser needs. The old `ScopedFileContext`
/// (which saved and restored the context stack across recursive parses) is
/// gone — under the one-file-per-call design there is no recursion to save
/// state across, and the context stack's emptiness at file boundaries is
/// asserted directly in parseOneFile.
struct ScopedDiagnosticFile {
    ScopedDiagnosticFile(ParserContext& ctx, InternedString file)
        : ctx_(ctx)
        , saved_(ctx.session.diagnostics.currentFile())
    {
        ctx_.session.diagnostics.setCurrentFile(file);
    }

    ~ScopedDiagnosticFile() {
        ctx_.session.diagnostics.setCurrentFile(saved_);
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