/// @file CompilationSession.hpp
/// @brief The per-compilation state: string pool, diagnostics, AST arena.
///
/// A CompilationSession owns the three pieces of state that every stage of
/// the compiler pipeline needs and that share a lifetime:
///
///   • StringPool          — canonical string storage for the session
///   • DiagnosticEngine    — collected errors and warnings for the session
///   • ASTArena            — bump-allocated storage for the session's AST
///
/// The pipeline stages — module resolution, parsing, Sema, bytecode
/// compilation — each take a `CompilationSession&` and reach the pool,
/// the diagnostics, and the arena through it. There is no global state and
/// no hidden singleton: two sessions in one process do not share anything.
///
/// ─── Lifetime ─────────────────────────────────────────────────────────────
/// One session per compilation. The CLI constructs one for `lucid run`,
/// `lucid parse`, `lucid sema`, and `lucid compile`; the LSP constructs a
/// fresh one for each file change; hot reload constructs a fresh one for
/// each watcher event. The session dies when the compilation completes and
/// everything it owns — every interned string, every collected diagnostic,
/// every AST node — is reclaimed with it.
///
/// ─── Non-copyable, non-movable ────────────────────────────────────────────
/// The diagnostic engine holds a pointer to the session's own pool. If the
/// session could be moved, that pointer would dangle after the move. The
/// copy and move constructors are deleted for that reason.
///
/// ─── Member order ─────────────────────────────────────────────────────────
/// The pool is declared first so it is constructed before the diagnostic
/// engine, which takes its address. The arena has no dependencies and is
/// declared last. Reordering the members would break the engine's
/// initializer; do not.
///
/// ─── What this file is not ────────────────────────────────────────────────
/// This is not the pipeline driver. It holds state; it does not know about
/// modules, ASTs, or bytecode. The CLI and the LSP drive the pipeline and
/// pass the session by reference. Keeping the session's surface small
/// (three members, no behavior) keeps it easy to reason about and easy to
/// test.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace lucid {

struct CompilationSession {
    /// Canonical string storage. Declared first so it is constructed
    /// before the diagnostic engine, which takes its address.
    StringPool pool;

    /// Collected diagnostics. Initialized with a pointer to this session's
    /// pool so interned-string payloads render with their actual text.
    diag::DiagnosticEngine diagnostics{&pool};

    /// Bump-allocated storage for the session's AST. Declared last; it has
    /// no dependencies on the other two.
    ASTArena arena;

    CompilationSession() = default;

    CompilationSession(const CompilationSession&)            = delete;
    CompilationSession& operator=(const CompilationSession&) = delete;
    CompilationSession(CompilationSession&&)                 = delete;
    CompilationSession& operator=(CompilationSession&&)      = delete;
};

} // namespace lucid