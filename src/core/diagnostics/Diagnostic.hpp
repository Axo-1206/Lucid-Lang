/**
 * @file Diagnostic.hpp
 *
 * @responsibility Procedural diagnostic interface for the compiler.
 *                 All functions are inside the `diagnostic` namespace.
 *
 * @usecase Called by the lexer, parser, semantic passes, and backend to
 *          report errors, warnings, and notes. Diagnostics are collected in
 *          one whole-session list and can be queried or dumped at any point,
 *          including at the very end from `main.cpp`.
 *
 * @architectural_note Namespace, not class — and why
 *   `ParserContext` and `SemaContext` are threaded explicitly through every
 *   parse/analysis function because parsing and analysis genuinely need an
 *   object per *session* (their own arena, string pool, module list, ...).
 *   Diagnostics don't have that shape: `main.cpp` will look roughly like
 *
 *   ```cpp
 *   auto modules = parseProgram(rootPath, rootSource, parserCtx);
 *   for (auto* m : modules) Sema::analyze(m, semaCtx);
 *   if (diagnostic::hasErrors()) { diagnostic::dumpAll(pool); return 1; }
 *   ```
 *
 *   — a single, final query made from a place that never touched (and
 *   shouldn't need to touch) a `ParserContext` or `SemaContext` instance.
 *   Making `diagnostic` a class would force `main.cpp` to either construct
 *   its own instance (disconnected from the one `Parser.cpp`/`Sema.cpp`
 *   reported into) or thread a shared instance through both phases just to
 *   ask one yes/no question at the end. A namespace over shared statics
 *   avoids that entirely: everything reports into the same place, and
 *   anything can ask about it without being handed a reference first.
 *
 * @architectural_note Single-threaded, on purpose
 *   The compiler's pipeline is strictly sequential — parsing must fully
 *   finish before semantic analysis can begin, module N's analysis (via
 *   `parseProgram()`'s dependency ordering) finishes before module N+1's
 *   begins — so there is exactly one diagnostic-producing activity at any
 *   given moment. Plain `static` globals (not `thread_local`) are
 *   deliberate, not an oversight: a future multi-threaded compiler (e.g.
 *   parsing independent modules in parallel) would need this made
 *   thread-local or turned into an explicit per-thread context, but that's
 *   a real design change for that future, not a defensive complication
 *   worth paying for today.
 *
 * @architectural_note The source-scope stack — replacing ParserContext's
 *   and SemaContext's duplicated bookkeeping
 *
 *   Before this rewrite, `ParserContext` and `SemaContext` each
 *   independently maintained their own `errors` vector, `hasErrors` flag,
 *   and `consecutiveErrors` counter — reset per file (`ScopedFileContext`)
 *   or per module (`ScopedModuleContext`), then drained into a separate
 *   `allDiagnostics` vector on the way out. That's the same bookkeeping,
 *   written twice, and the lexer would have needed a third copy.
 *
 *   The fix: the global `diagnostics` list (see Diagnostic.cpp) is now the
 *   ONLY place diagnostics live — nothing copies them elsewhere, and
 *   nothing needs to "drain" anything into a session-wide list, because
 *   they were already in it the moment they were reported. What
 *   `ScopedFileContext`/`ScopedModuleContext` actually needed on top of
 *   that was a way to answer "how many errors, and how many in a row,
 *   happened *since I started working on this specific file/module*" — and
 *   that's what `pushSource()`/`popSource()` (prefer `ScopedSource`)
 *   provide: each pushed frame just remembers the index into `diagnostics`
 *   at the moment it was pushed, plus its own local counters. "This file's
 *   diagnostics" becomes a slice of one list, not a second list.
 *
 *   A useful side effect: nesting falls out for free. When parsing an
 *   `import` recurses into another file, that file's `ScopedSource` frame
 *   sits on top of the importer's. Errors reported while the import is
 *   being parsed only increment the *innermost* frame — the importer's own
 *   frame is untouched underneath, and reappears exactly as it was the
 *   moment the inner frame is popped. `ParserContext` used to implement
 *   this by hand (explicitly saving and restoring `consecutiveErrors`
 *   around every nested `parse()` call); with a real stack of independent
 *   frames, "the frame underneath is unaffected" isn't something anyone
 *   has to remember to implement — it's just what a stack does.
 *
 * @architectural_note What "consecutive" means here
 *   A pushed frame's `consecutiveErrors` counts Error reports made
 *   while it is the innermost frame, since it was pushed (or since the last
 *   `resetStreak()` call). It does NOT reset itself just because a Warning
 *   or Note was reported in between — Diagnostic has no way to know "the
 *   parser made progress" on its own, only "a diagnostic was reported."
 *   `resetStreak()` exists as an opt-in hook for a caller that DOES know
 *   that (e.g. a parser loop that successfully parsed another declaration)
 *   and wants a true "no progress in N tries" signal rather than a simple
 *   running total. Passing no such hook at all reproduces exactly what
 *   `ParserContext::consecutiveErrors` measured before this rewrite: a
 *   bounded total for the current file/module, used by `canContinue()` to
 *   bail out of a run that's accumulating far more errors than a real
 *   source file plausibly should.
 *
 *   Warnings never increment this counter (that was the bug this rewrite
 *   started from) — a warning is neither a failure nor evidence of one.
 */

#pragma once

#include "DiagnosticCodes.hpp"
#include "../SourceLocation.hpp"
#include "../memory/InternedString.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <initializer_list>
#include <ostream>
#include <cstddef>

// Forward declarations for types used in API
class StringPool;

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticSeverity  — Categorizes the impact of the diagnostic
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagnosticSeverity {
    Note,    ///< Informational message or a hint.
    Warning, ///< Potential issue that doesn't stop the build.
    Error,   ///< Requirement violation that prevents a successful build.
};

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticCategory  — Identifies which compiler sub‑system emitted the diagnostic
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagnosticCategory {
    General,  ///< Misc or driver errors or file/module problem
    Lexical,  ///< Scanner/tokenization errors.
    Syntax,   ///< Parser/grammar errors.
    Semantic, ///< Type‑checking or logic errors.
    Backend   ///< LLVM IR generation or optimization errors.
};

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic  — The data packet for a single reported issue
// ─────────────────────────────────────────────────────────────────────────────
struct Diagnostic {
    DiagnosticSeverity severity;          ///< Impact level (Error, Warning, etc.).
    DiagnosticCategory category;          ///< Compiler phase (Lexical, Syntax, etc.).
    InternedString file;                  ///< Source file path (interned).
    SourceLocation location;              ///< Line/column within the file.
    DiagCode code;                        ///< Unique numeric error code.
    std::vector<std::string> args;        ///< Format arguments for %s placeholders.
};

// =============================================================================
// Namespace `diagnostic` – Procedural API for all compiler diagnostics
// =============================================================================

namespace diagnostic {

// -----------------------------------------------------------------------------
// Quick Reference — "which function do I actually want?"
// -----------------------------------------------------------------------------
//
//   "Did anything go wrong at all?"            → hasErrors()
//   "Did anything go wrong in THIS file/module (right after parsing/
//    analyzing it, while its ScopedSource is still open)?"
//                                               → hasErrorsInCurrentSource()
//   "Just the numbers, for a log line"          → totalErrorCount() /
//                                                  totalWarningCount()
//   "Full rollup: counts + which files"         → summarize()
//   "Every diagnostic for one specific file"    → getAllForFile(path)
//   "Every diagnostic, unfiltered"              → getAll()
//   "Should I keep going, or bail out?"         → canContinue()
//
// The common end-of-pipeline check in main.cpp is just:
//
//   if (diagnostic::hasErrors()) {
//       auto summary = diagnostic::summarize();
//       std::cerr << summary.errorCount << " error(s) in "
//                 << summary.filesWithErrors.size() << " file(s)\n";
//       diagnostic::dumpAll(pool);
//       return 1;
//   }

// -----------------------------------------------------------------------------
// Reporting functions — explicit file
// -----------------------------------------------------------------------------
//
// These remain the primitive, fully-explicit form: useful for a diagnostic
// that's genuinely ABOUT a different file than the one currently active
// (e.g. semantic analysis of module Y reporting that a trait requirement
// declared in module X isn't satisfied — the message should point at X's
// declaration even while Y is the "current source"). For the common case
// of reporting against whatever is currently being processed, prefer the
// implicit-file overloads below.

/**
 * @brief Report an error.
 * @param category Which compiler phase detected the issue.
 * @param file     Source file path (interned).
 * @param loc      Source location (line, column).
 * @param code     Diagnostic code (e.g., DiagCode::E2001).
 * @param args     Format arguments for %s placeholders in the message template.
 *
 * @note Always increments the innermost open source frame's error count and
 *       consecutive-error streak (see pushSource()), regardless of which
 *       `file` is named here — the frame tracks "what's currently being
 *       worked on," not "what this specific message is about."
 */
void error(DiagnosticCategory category, InternedString file,
           SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args = {});

/**
 * @brief Report a warning.
 * @param category Which compiler phase detected the issue.
 * @param file     Source file path (interned).
 * @param loc      Source location (line, column).
 * @param code     Diagnostic code (e.g., DiagCode::W0002).
 * @param args     Format arguments for %s placeholders in the message template.
 *
 * @note Does NOT affect the innermost frame's consecutive-error streak.
 */
void warning(DiagnosticCategory category, InternedString file,
             SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args = {});

/**
 * @brief Report a free‑text note (not tied to a diagnostic code).
 * @param file Source file path (interned).
 * @param loc  Source location.
 * @param msg  Human‑readable message, printed verbatim (see dumpAll() —
 *             unlike error()/warning(), a note's message is never run
 *             through the DiagCode template lookup).
 *
 * @note Affects neither the error/warning counters nor any frame's streak.
 */
void note(InternedString file, SourceLocation loc, const std::string& msg);

// -----------------------------------------------------------------------------
// Reporting functions — implicit current source
// -----------------------------------------------------------------------------
//
// Equivalent to the explicit-file overloads above with `file` filled in from
// currentFile() (the innermost pushSource() frame, or an empty
// InternedString if nothing is currently pushed). This is what call sites
// inside the parser/sema passes should normally use once they're calling
// through a live ScopedSource, the same way ParserContext::error()/
// SemaContext::error() used to hide this detail behind their own methods.

void error(DiagnosticCategory category, SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args = {});
void warning(DiagnosticCategory category, SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args = {});
void note(SourceLocation loc, const std::string& msg);

// -----------------------------------------------------------------------------
// Whole-session queries
// -----------------------------------------------------------------------------

/// Returns true if at least one Error has been reported, across the
/// entire session (every file/module, not just the current source scope).
/// This is what `main.cpp` should check right before exiting.
bool hasErrors();

/// Returns true if at least one Warning has been reported, across the
/// entire session.
bool hasWarnings();

/// Total number of Error diagnostics reported across the entire session.
/// This is the counter `hasErrors()` already checks against zero — exposed
/// directly for logging ("Found 7 errors") rather than just a yes/no.
int totalErrorCount();

/// Total number of Warning diagnostics reported across the entire session.
int totalWarningCount();

/**
 * @brief A point-in-time rollup of the whole session's diagnostics.
 *
 * Built for exactly the "N errors across M files" logging use case: call
 * this once after parsing/analysis finishes (no open ScopedSource
 * required — this reads the whole `diagnostics` list, not a frame) and log
 * from the result directly, e.g.:
 *
 * ```cpp
 * auto summary = diagnostic::summarize();
 * std::cerr << summary.errorCount << " error(s) in "
 *           << summary.filesWithErrors.size() << " file(s)\n";
 * ```
 *
 * No upfront "registration" of files is needed anywhere in this API —
 * see summarize()'s own doc comment.
 */
struct DiagnosticSummary {
    int errorCount = 0;
    int warningCount = 0;

    /// Distinct files with at least one Error, in first-encountered order.
    std::vector<InternedString> filesWithErrors;

    /// Distinct files with at least one Warning, in first-encountered order.
    std::vector<InternedString> filesWithWarnings;
};

/**
 * @brief Compute a DiagnosticSummary from the whole session's diagnostics.
 *
 * A file shows up in `filesWithErrors`/`filesWithWarnings` purely because
 * at least one diagnostic was reported with that `.file` value — there is
 * no separate "list of files known to the diagnostic system" to keep in
 * sync, and nothing needs to be registered before diagnostics for it can
 * be reported or found. A file that parsed cleanly simply never appears in
 * either list; a file that only ever appears as the target of a
 * cross-file diagnostic (see the explicit-file overloads) still shows up
 * correctly even though no ScopedSource was ever pushed for it.
 */
DiagnosticSummary summarize();

/**
 * @brief Clear every collected diagnostic and reset all counters, including
 *        the source-scope stack.
 *
 * Intended for test isolation between independent compiler runs in the same
 * process — NOT for use while a `ScopedSource` is alive. Clearing
 * `diagnostics` out from under an open frame invalidates that frame's
 * remembered start index; the frame is forcibly cleared here too rather
 * than left dangling, but a caller that does this mid-parse will simply
 * lose whatever scoping was in progress. This function cannot detect that
 * misuse — it's a caller contract, the same way `ParserContext::clearErrors()`
 * was never meant to be called out from under an open `ScopedFileContext`.
 */
void clear();

/// Returns the full list of diagnostics across the whole session (useful
/// for testing or serialisation).
const std::vector<Diagnostic>& getAll();

/// Returns every diagnostic reported against `file`, regardless of which
/// source scope was active when each was reported. Unlike
/// currentSourceDiagnostics() (below), this works after the fact — no
/// ScopedSource needs to be open for `file` right now.
std::vector<Diagnostic> getAllForFile(InternedString file);

// -----------------------------------------------------------------------------
// Source scope — one frame per file (parser) or module (sema) currently
// being processed. See this file's own "source-scope stack" architecture
// note above for the full rationale.
// -----------------------------------------------------------------------------

/**
 * @brief Push a new source-scope frame for `file`.
 *
 * Prefer constructing a `ScopedSource` over calling this directly — see its
 * doc comment for why (balancing push/pop by hand across every early return
 * is exactly the class of bug RAII exists to prevent, and is the same
 * reasoning `ScopedParsingGuard` documents for `ModuleResolver`).
 */
void pushSource(InternedString file);

/// Pop the innermost source-scope frame. Prefer `ScopedSource`'s destructor.
void popSource();

/// The innermost open frame's file, or an empty InternedString if no frame
/// is currently open.
InternedString currentFile();

/// True if any Error has been reported while the innermost frame has
/// been open. False if no frame is open. This is the per-file/per-module
/// equivalent of `ModuleAST::hasErrors` — read this right after the
/// corresponding `parse()`/`Sema::analyze()` call to fill it in, the same
/// point `thisModule->hasErrors` used to be assigned from
/// `ctx.hasErrors` directly.
bool hasErrorsInCurrentSource();

/**
 * @brief The innermost frame's current consecutive-error count.
 *
 * @warning "Consecutive" here means what you get out of it depends on
 *          whether the caller uses `resetStreak()`. Without it, this is a
 *          running total of every Error reported since the frame was
 *          pushed — 5 errors scattered across an otherwise-successful file,
 *          with hundreds of successfully-parsed declarations in between,
 *          count the same as 5 errors reported back-to-back with nothing
 *          else happening. Diagnostic has no way to tell those apart on
 *          its own — it only ever sees "a diagnostic was reported," never
 *          "the parser/checker made progress." If you want the threshold
 *          in `canContinue()` to mean genuinely consecutive — i.e. "bail
 *          out only after N failures in a row with no successful
 *          declaration/statement in between, and keep going no matter how
 *          many total errors there are as long as they're not bunched
 *          up" — the caller MUST call `resetStreak()` at its own
 *          "progress" points. See `resetStreak()`'s doc comment for the
 *          exact pattern.
 *
 * 0 if no frame is open.
 */
int consecutiveErrorsInCurrentSource();

/**
 * @brief True if the innermost frame's consecutive-error count is still
 *        below `threshold`.
 *
 * Direct replacement for `ParserContext::canContinue()` /
 * `SemaContext::canContinue()` — now backed by one shared counter instead
 * of two independently-maintained (and, in one case, buggy) ones. Returns
 * true (unbounded) if no frame is open, since there's nothing to bound.
 *
 * Whether "10 errors" here means "10 total this file" or "10 in a row with
 * no progress in between" is entirely determined by whether the caller
 * calls `resetStreak()` — see `consecutiveErrorsInCurrentSource()`'s
 * @warning and `resetStreak()`'s own doc comment. Neither behavior is
 * "more correct" in the abstract; a caller that never got around to
 * calling `resetStreak()` still gets a real, useful safety net (a hard
 * cap on total errors per file), just a blunter one than true
 * back-to-back detection.
 */
bool canContinue(int threshold = 10);

/**
 * @brief Reset the innermost frame's consecutive-error count to zero.
 *
 * This is what makes `canContinue()` mean "N consecutive failures with no
 * progress in between" rather than "N total errors this file" — call it
 * at whatever point the caller considers "progress was made." Calling
 * this is never required; without it, `consecutiveErrorsInCurrentSource()`
 * is simply a running total for the current frame, which is still a
 * sufficient (if blunter) safety net for `canContinue()`'s
 * runaway-failure use case on its own.
 *
 * ## Worked example: true consecutive-only tracking in a parse loop
 *
 * ```cpp
 * void parseInternal(TokenStream& stream, ParserContext& ctx,
 *                     std::vector<DeclPtr>& outDecls) {
 *     while (!stream.isAtEnd()) {
 *         if (!diagnostic::canContinue()) break;   // N in a row, no progress — give up
 *
 *         size_t before = diagnostic::totalErrorCount();
 *         DeclAST* decl = parseDecl(stream, ctx);
 *         outDecls.push_back(decl);
 *
 *         if (diagnostic::totalErrorCount() == before) {
 *             // No new error came out of parsing that declaration —
 *             // genuine progress. Forgive whatever streak had built up
 *             // from earlier, unrelated failures.
 *             diagnostic::resetStreak();
 *         }
 *         // If a new error DID come out, consecutiveErrors is already
 *         // incremented (add() did that at report time) — nothing else
 *         // to do here; the streak simply continues.
 *     }
 * }
 * ```
 *
 * Without the `resetStreak()` call above, the exact same loop still works
 * and is still safe to run — `canContinue()` just caps total errors for
 * the file instead of capping unbroken runs of them.
 */
void resetStreak();

/**
 * @brief Every diagnostic reported since the innermost frame was pushed.
 *
 * A general-purpose tool for a caller that wants this file/module's own
 * diagnostics as a plain list right now — e.g. handing them to an external
 * tool (LSP, test harness) that expects a `std::vector<Diagnostic>`
 * directly rather than making its own `getAllForFile()` call. For most
 * purposes, prefer `getAllForFile(module->filePath)` after the fact
 * instead (see this file's Quick Reference note) — it works identically
 * whether or not a `ScopedSource` is still open, and doesn't require the
 * caller to have grabbed this at exactly the right moment. Returns empty
 * if no frame is open.
 */
std::vector<Diagnostic> currentSourceDiagnostics();

/**
 * @brief RAII guard for `pushSource()`/`popSource()`.
 *
 * Pushes a new source-scope frame on construction, pops it on destruction —
 * automatically, on every exit path. Direct replacement for what
 * `ScopedFileContext` (parser) and `ScopedModuleContext` (sema) used to do
 * by hand for their own local `errors`/`hasErrors`/`consecutiveErrors`
 * fields; those structs still exist for THEIR OWN state (the parser's
 * `contextStack` of grammar nesting, sema's `scopes`/`definingTypeStack`,
 * ...) but no longer need to duplicate diagnostic bookkeeping alongside it.
 *
 * ```cpp
 * ModuleAST* parse(const std::string& path, const std::string& source, ParserContext& ctx) {
 *     diagnostic::ScopedSource sourceScope(ctx.pool.intern(path));
 *     ScopedFileContext fileContext(ctx);   // still resets ctx's OWN state
 *     ...
 *     thisModule->hasErrors = diagnostic::hasErrorsInCurrentSource();
 *     return thisModule;
 *     // sourceScope pops here — this file's frame is gone, the importer's
 *     // (if any) reappears underneath exactly as it was left. This
 *     // module's own diagnostics are still retrievable afterward via
 *     // diagnostic::getAllForFile(thisModule->filePath) — see BaseAST.hpp's
 *     // ModuleAST doc comment for why the node itself no longer stores them.
 * }
 * ```
 *
 * Non-copyable, non-movable: identity is tied to one specific activation,
 * the same reasoning every other Scoped* guard in this codebase documents.
 */
struct ScopedSource {
    explicit ScopedSource(InternedString file) {
        pushSource(file);
    }

    ~ScopedSource() {
        popSource();
    }

    ScopedSource(const ScopedSource&) = delete;
    ScopedSource& operator=(const ScopedSource&) = delete;
    ScopedSource(ScopedSource&&) = delete;
    ScopedSource& operator=(ScopedSource&&) = delete;
};

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------
//
// @architectural_note Diagnostic is data, not a renderer
//   `Diagnostic` (the struct) carries only structured information —
//   severity, category, file, location, code, args — and nothing here
//   changes that. `formatOneLine()` and `dumpAll()` are ONE particular
//   rendering of that data, opinionated for a plain-text terminal, kept
//   deliberately separate from and no more privileged than any other
//   renderer a consumer might write. An LSP server or editor integration
//   should NOT parse `formatOneLine()`'s output — it should build its own
//   `Diagnostic` → protocol mapping directly from the struct fields (via
//   `getAll()`/`getAllForFile()`), the same way `formatOneLine()` itself
//   does. If a second text format is ever needed (say, machine-readable
//   JSON lines for a build tool), it belongs here as its own function
//   reading the same structured data — not as a configuration knob bolted
//   onto `formatOneLine()`.

/**
 * @brief Format a single diagnostic as one line, in the style of an
 *        editor's "Problems" panel:
 *
 *        [ERROR] E1002: Expected an identifier  src/main.lucid:12:5
 *        [WARNING] W0002: Unreachable code  src/main.lucid:30:1
 *        [NOTE] Consider using 'let' instead  src/main.lucid:12:5
 *
 * The 4-digit number after the severity prefix is the code's position
 * within its own range as named in DiagnosticCodes.hpp (e.g. `E1002` is
 * the 2nd code after `E1001 = 1000`; `W0002` is the 2nd code after
 * `W0001 = 6000`) — not the raw underlying `DiagCode` integer, which is
 * an implementation detail nothing outside this file should need to
 * reason about. Notes never carry a code (see note()'s own doc comment),
 * so the `CODE:` segment is omitted for them.
 *
 * @param diag The diagnostic to format.
 * @param pool StringPool for looking up `diag.file`.
 */
std::string formatOneLine(const Diagnostic& diag, const StringPool& pool);

/**
 * @brief Print all collected diagnostics to an output stream, one line each
 *        (via formatOneLine()), followed by a summary line built from
 *        summarize() — e.g. "3 error(s), 1 warning(s) generated across 2 file(s)".
 * @param pool StringPool for looking up interned file paths.
 * @param os   Output stream (defaults to std::cerr).
 */
void dumpAll(const StringPool& pool, std::ostream& os = std::cerr);

} // namespace diagnostic