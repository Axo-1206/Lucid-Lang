/**
 * @file Diagnostic.hpp
 * @brief Procedural diagnostic interface for the compiler.
 *
 * This is the SINGLE header for all diagnostic functionality.
 * It combines:
 *   - Diagnostic data types (Diagnostic, Summary)
 *   - Reporting API (error, warning, note, hint)
 *   - Source scope management (ScopedSource, pushSource, etc.)
 *   - Query API (hasErrors, totalErrorCount, summarize, etc.)
 *   - Formatting API (formatOneLine, dumpAll, etc.)
 *
 * @architectural_note Single header design
 *   All diagnostic functionality is in one header to eliminate:
 *   - Circular dependencies between DiagnosticSink and DiagnosticFormatter
 *   - Duplicate includes across multiple files
 *   - Confusion about which header to include
 *
 * @architectural_note Severity as numeric levels
 *   Uses integer levels (0-4) compatible with LSP and editor tooling:
 *   0 = Hint, 1 = Note, 2 = Warning, 3 = Error, 4 = Fatal
 *
 * @architectural_note No DiagnosticCategory
 *   The category is derived from the error code range, not stored separately.
 *
 * @architectural_note Source-scope stack
 *   Each frame remembers an index into the global diagnostics list plus
 *   its own counters. "This file's diagnostics" is a slice of one list.
 */

#pragma once

#include "DiagnosticTypes.hpp"
#include "DiagnosticCodes.hpp"
#include "../SourceLocation.hpp"
#include "../memory/InternedString.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <initializer_list>
#include <unordered_set>
#include <ostream>

namespace diagnostic {

// =============================================================================
// Quick Reference — "which function do I actually want?"
// =============================================================================
//
//   "Did anything go wrong at all?"            → hasErrors()
//   "Did anything go wrong in THIS file?"      → hasErrorsInCurrentSource()
//   "Just the numbers, for a log line"         → totalErrorCount()
//   "Full rollup: counts + which files"        → summarize()
//   "Every diagnostic for one specific file"   → getAllForFile(file)
//   "Every diagnostic, unfiltered"             → getAll()
//   "Should I keep going, or bail out?"        → canContinue()

// =============================================================================
// Reporting API
// =============================================================================

// ─── Explicit file ────────────────────────────────────────────────────────

/**
 * @brief Report an error.
 * @param file Source file path (interned).
 * @param loc  Source location (line, column).
 * @param code Diagnostic code.
 * @param args Format arguments for %s placeholders.
 *
 * @note Always increments the innermost open source frame's error count and
 *       consecutive-error streak, regardless of which `file` is named.
 */
void error(InternedString file, SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args = {});

/**
 * @brief Report a warning.
 * @param file Source file path (interned).
 * @param loc  Source location.
 * @param code Diagnostic code.
 * @param args Format arguments for %s placeholders.
 *
 * @note Does NOT affect the innermost frame's consecutive-error streak.
 */
void warning(InternedString file, SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args = {});

/**
 * @brief Report a free-text note (not tied to a diagnostic code).
 * @param file Source file path (interned).
 * @param loc  Source location.
 * @param msg  Human-readable message, printed verbatim.
 *
 * @note Affects neither the error/warning counters nor any frame's streak.
 */
void note(InternedString file, SourceLocation loc, const std::string& msg);

/**
 * @brief Report a free-text hint (not tied to a diagnostic code).
 * @param file Source file path (interned).
 * @param loc  Source location.
 * @param msg  Human-readable hint, printed verbatim.
 */
void hint(InternedString file, SourceLocation loc, const std::string& msg);

// ─── Implicit current source ─────────────────────────────────────────────

/// Report an error at the current source.
void error(SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args = {});

/// Report a warning at the current source.
void warning(SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args = {});

/// Report a free-text note at the current source.
void note(SourceLocation loc, const std::string& msg);

/// Report a free-text hint at the current source.
void hint(SourceLocation loc, const std::string& msg);

// =============================================================================
// Query API
// =============================================================================

/// True if at least one Error or Fatal has been reported.
bool hasErrors();

/// True if at least one Warning has been reported.
bool hasWarnings();

/// Total number of Error + Fatal diagnostics.
int totalErrorCount();

/// Total number of Warning diagnostics.
int totalWarningCount();

/// Total number of Note diagnostics.
int totalNoteCount();

/// Total number of Hint diagnostics.
int totalHintCount();

/// Compute a rollup summary of all diagnostics.
Summary summarize();

/// Clear every collected diagnostic and reset all counters (testing only).
void clear();

/// Returns the full list of diagnostics.
const std::vector<Diagnostic>& getAll();

/// Returns every diagnostic reported against `file`.
std::vector<Diagnostic> getAllForFile(InternedString file);

// =============================================================================
// Source Scope API
// =============================================================================

/**
 * @brief Push a new source-scope frame for `file`.
 *
 * Prefer constructing a ScopedSource over calling this directly.
 */
void pushSource(InternedString file);

/// Pop the innermost source-scope frame. Prefer ScopedSource's destructor.
void popSource();

/// The innermost open frame's file, or an empty InternedString.
InternedString currentFile();

/// True if any Error has been reported in the current source.
bool hasErrorsInCurrentSource();

/// The innermost frame's current consecutive-error count.
int consecutiveErrorsInCurrentSource();

/**
 * @brief True if the innermost frame's consecutive-error count is below threshold.
 *
 * @param threshold Maximum allowed consecutive errors (default: 10).
 */
bool canContinue(int threshold = 10);

/// Reset the innermost frame's consecutive-error count to zero.
void resetStreak();

/// Every diagnostic reported since the innermost frame was pushed.
std::vector<Diagnostic> currentSourceDiagnostics();

// =============================================================================
// RAII Guard
// =============================================================================

/**
 * @brief RAII guard for pushSource()/popSource().
 *
 * Pushes a new source-scope frame on construction, pops it on destruction.
 */
struct ScopedSource {
    explicit ScopedSource(InternedString file);
    ~ScopedSource();

    ScopedSource(const ScopedSource&) = delete;
    ScopedSource& operator=(const ScopedSource&) = delete;
    ScopedSource(ScopedSource&&) = delete;
    ScopedSource& operator=(ScopedSource&&) = delete;

    /// Get the file associated with this scope.
    InternedString file() const { return m_file; }

private:
    InternedString m_file;
};

// =============================================================================
// Formatting API
// =============================================================================

/**
 * @brief Format a single diagnostic as one line.
 *
 * Output format:
 *   [ERROR] E2001: undefined value 'foo'  src/main.luc:12:5
 *   [NOTE] Consider using 'let' instead  src/main.luc:12:5
 *
 * @note Uses StringPool::instance() for string lookup.
 */
std::string formatOneLine(const Diagnostic& diag);

/**
 * @brief Format a diagnostic for terminal output with colors.
 * Uses ANSI color codes for severity highlighting.
 */
std::string formatOneLineWithColor(const Diagnostic& diag);

/**
 * @brief Format a diagnostic as a structured JSON object.
 * Useful for LSP and tooling integration.
 */
std::string formatJSON(const Diagnostic& diag);

/**
 * @brief Print all collected diagnostics to an output stream.
 * @param os Output stream (defaults to std::cerr).
 */
void dumpAll(std::ostream& os = std::cerr);

/**
 * @brief Print all collected diagnostics with ANSI color codes.
 * @param os Output stream (defaults to std::cerr).
 */
void dumpAllWithColor(std::ostream& os = std::cerr);

} // namespace diagnostic