/**
 * @file DiagnosticTypes.hpp
 * @brief Diagnostic data structures.
 *
 * Contains the core data types: Diagnostic, Summary, and related helpers.
 * This file has minimal dependencies to avoid cyclic includes.
 */

#pragma once

#include "DiagnosticCodes.hpp"
#include "DiagnosticSeverity.hpp"
#include "../SourceLocation.hpp"
#include "../memory/InternedString.hpp"

#include <vector>
#include <string>
#include <string_view>

namespace diagnostic {

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic Data
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A single diagnostic report.
 *
 * Contains all information needed for formatting and tooling integration.
 * The `category` is derived from the code range, not stored separately.
 */
struct Diagnostic {
    Severity severity;              ///< Severity level (0-4)
    InternedString file;            ///< Source file path (interned)
    SourceLocation location;        ///< Line/column within the file
    DiagCode code;                  ///< Unique numeric error code
    std::vector<std::string> args;  ///< Format arguments for %s placeholders

    /**
     * @brief Get the category from the code range.
     *
     *   - 0000-0099: Environment/Driver
     *   - 0100-0999: Lexical
     *   - 1000-1999: Syntax (Parser)
     *   - 2000-2999: Semantic — Name Resolution
     *   - 3000-3999: Semantic — Type Checking
     *   - 4000-4999: Semantic — Generics/Traits/FFI
     *   - 5000-5999: Backend / Linker
     *   - 6000-6999: Warnings (cross-cutting)
     */
    std::string_view category() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic Summary
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Rollup of diagnostics for reporting.
 */
struct Summary {
    int errorCount = 0;         ///< Total errors (Error + Fatal)
    int warningCount = 0;       ///< Total warnings
    int noteCount = 0;          ///< Total notes
    int hintCount = 0;          ///< Total hints

    /// Files with at least one error
    std::vector<InternedString> filesWithErrors;

    /// Files with at least one warning
    std::vector<InternedString> filesWithWarnings;

    /**
     * @brief Total distinct files with any diagnostic.
     */
    size_t totalFilesWithDiagnostics() const;
};

} // namespace diagnostic