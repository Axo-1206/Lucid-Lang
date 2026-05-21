/**
 * @file DiagnosticEngine.hpp
 * 
 * @responsibility Central collector and reporter for all compiler diagnostics.
 * 
 * @usecase Owned by the Compiler driver and shared across Parser and Semantic phases.
 *
 * @note Messages are stored statically in DiagnosticMessages.cpp and looked up by code.
 *       Format arguments (%s placeholders) are stored separately and formatted on dump.
 */

#pragma once

#include "Diagnostic.hpp"
#include "DiagnosticCodes.hpp"
#include "ast/support/StringPool.hpp"
#include <vector>
#include <iostream>
#include <initializer_list>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticEngine  — Manages the collection and reporting of all compiler issues
//
// The engine acts as a central repository for Diagnostics. It tracks the 
// total error count to allow the driver to decide whether to stop 
// compilation after a phase finishes.
// ─────────────────────────────────────────────────────────────────────────────
class DiagnosticEngine {
public:
    DiagnosticEngine() = default;

    // ─────────────────────────────────────────────────────────────────────────
    // report  — Report a new diagnostic to the engine (full severity)
    // ─────────────────────────────────────────────────────────────────────────
    void report(DiagnosticSeverity severity, DiagnosticCategory category,
                InternedString file, SourceLocation loc, DiagCode code,
                std::initializer_list<std::string> args = {});

    // ─────────────────────────────────────────────────────────────────────────
    // error  — Convenience shorthand for reporting an Error
    // ─────────────────────────────────────────────────────────────────────────
    void error(DiagnosticCategory category, InternedString file, SourceLocation loc,
               DiagCode code, std::initializer_list<std::string> args = {});

    // ─────────────────────────────────────────────────────────────────────────
    // warning  — Convenience shorthand for reporting a Warning
    // ─────────────────────────────────────────────────────────────────────────
    void warning(DiagnosticCategory category, InternedString file, SourceLocation loc,
                 DiagCode code, std::initializer_list<std::string> args = {});

    // ─────────────────────────────────────────────────────────────────────────
    // note  — Convenience for informational notes (usually chained after an error)
    // ─────────────────────────────────────────────────────────────────────────
    void note(InternedString file, SourceLocation loc, const std::string& msg);

    // ─────────────────────────────────────────────────────────────────────────
    // hasErrors  — Check if any 'Error' or 'Fatal' diagnostics have been recorded
    // ─────────────────────────────────────────────────────────────────────────
    bool hasErrors() const { return errorCount_ > 0; }

    // ─────────────────────────────────────────────────────────────────────────
    // hasWarnings  — Check if any Warning diagnostics have been recorded
    // ─────────────────────────────────────────────────────────────────────────
    bool hasWarnings() const { return warningCount_ > 0; }

    // ─────────────────────────────────────────────────────────────────────────
    // getDiagnostics  — Access the full list of collected diagnostics
    // ─────────────────────────────────────────────────────────────────────────
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }

    // ─────────────────────────────────────────────────────────────────────────
    // dumpAll  — Print all diagnostics to the console with basic formatting
    // ─────────────────────────────────────────────────────────────────────────
    void dumpAll(const StringPool& pool, std::ostream& os = std::cerr) const;

    // ─────────────────────────────────────────────────────────────────────────
    // clear  — Reset the engine (clear all diagnostics and counters)
    // ─────────────────────────────────────────────────────────────────────────
    void clear();

private:
    std::vector<Diagnostic> diagnostics_; ///< Internal storage for all reports.
    int errorCount_   = 0; ///< Number of issues with severity >= Error.
    int warningCount_ = 0; ///< Number of Warning-severity issues.

    // Helper to format a message with arguments
    std::string formatMessage(const Diagnostic& diag) const;
};