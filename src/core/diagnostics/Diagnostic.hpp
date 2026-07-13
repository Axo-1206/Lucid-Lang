/**
 * @file Diagnostic.hpp
 *
 * @responsibility Procedural diagnostic interface for the compiler.
 *                 All functions are inside the `diagnostic` namespace.
 *
 * @usecase Called by lexer, parser, semantic passes, and backend to report
 *          errors, warnings, and notes. Diagnostics are collected in a global
 *          list and can be dumped at the end of compilation.
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

// Forward declarations for types used in API
class StringPool;

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticSeverity  — Categorizes the impact of the diagnostic
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagnosticSeverity {
    Note,    ///< Informational message or a hint.
    Warning, ///< Potential issue that doesn't stop the build.
    Error,   ///< Requirement violation that prevents a successful build.
    Fatal    ///< Critical failure that causes the compiler to stop immediately.
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
// Reporting functions
// -----------------------------------------------------------------------------

/**
 * @brief Report an error.
 * @param category Which compiler phase detected the issue.
 * @param file     Source file path (interned).
 * @param loc      Source location (line, column).
 * @param code     Diagnostic code (e.g., DiagCode::E2001).
 * @param args     Format arguments for %s placeholders in the message template.
 */
void error(DiagnosticCategory category, InternedString file,
           SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args = {});

/**
 * @brief Report a warning.
 * @param category Which compiler phase detected the issue.
 * @param file     Source file path (interned).
 * @param loc      Source location (line, column).
 * @param code     Diagnostic code (e.g., DiagCode::W3001).
 * @param args     Format arguments for %s placeholders in the message template.
 */
void warning(DiagnosticCategory category, InternedString file,
             SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args = {});

/**
 * @brief Report a free‑text note (not tied to a diagnostic code).
 * @param file Source file path (interned).
 * @param loc  Source location.
 * @param msg  Human‑readable message.
 */
void note(InternedString file, SourceLocation loc, const std::string& msg);

// -----------------------------------------------------------------------------
// Query functions
// -----------------------------------------------------------------------------

/// Returns true if at least one Error or Fatal has been reported.
bool hasErrors();

/// Returns true if at least one Warning has been reported.
bool hasWarnings();

/// Clears all collected diagnostics and resets error/warning counters.
void clear();

/// Returns the full list of diagnostics (useful for testing or serialisation).
const std::vector<Diagnostic>& getAll();

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------

/**
 * @brief Print all collected diagnostics to an output stream.
 * @param pool StringPool for looking up interned file paths.
 * @param os   Output stream (defaults to std::cerr).
 */
void dumpAll(const StringPool& pool, std::ostream& os = std::cerr);

} // namespace diagnostic