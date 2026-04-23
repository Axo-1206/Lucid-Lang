/**
 * @file DiagnosticEngine.hpp
 * 
 * @responsibility Central collector and reporter for all compiler diagnostics.
 * 
 * @usecase Owned by the Compiler driver and shared across Parser and Semantic phases.
 */

#pragma once

#include "Diagnostic.hpp"
#include "DiagnosticCodes.hpp"
#include <vector>
#include <iostream>

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

    // ─────────────────────────────────────────────────────────────────────────────
    // report  — Report a new diagnostic to the engine
    //
    // Records an issue with full details including severity, category, source 
    // location, numeric code, and descriptive user message.
    // ─────────────────────────────────────────────────────────────────────────────
    void report(DiagnosticSeverity severity, DiagnosticCategory category,
                SourceLocation loc, DiagCode code, const std::string& msg);

    // ─────────────────────────────────────────────────────────────────────────────
    // error  — Convenience shorthand for reporting an Error
    //
    // Automatically delegates to report() setting the severity specifically to 
    // DiagnosticSeverity::Error.
    // ─────────────────────────────────────────────────────────────────────────────
    void error(DiagnosticCategory category, SourceLocation loc, DiagCode code, const std::string& msg);

    // ─────────────────────────────────────────────────────────────────────────────
    // warning  — Convenience shorthand for reporting a Warning
    //
    // Warnings do not block compilation. They are emitted to inform the developer
    // of suspicious but technically legal (or recoverable) situations.
    // ─────────────────────────────────────────────────────────────────────────────
    void warning(DiagnosticCategory category, SourceLocation loc, DiagCode code, const std::string& msg);

    // ─────────────────────────────────────────────────────────────────────────────
    // hasErrors  — Check if any 'Error' or 'Fatal' diagnostics have been recorded
    // ─────────────────────────────────────────────────────────────────────────────
    bool hasErrors() const { return errorCount_ > 0; }

    // ─────────────────────────────────────────────────────────────────────────────
    // hasWarnings  — Check if any Warning diagnostics have been recorded
    // ─────────────────────────────────────────────────────────────────────────────
    bool hasWarnings() const { return warningCount_ > 0; }

    // ─────────────────────────────────────────────────────────────────────────────
    // getDiagnostics  — Access the full list of collected diagnostics
    //
    // Returns a constant reference to the underlying vector holding every compiled 
    // issue logged thus far.
    // ─────────────────────────────────────────────────────────────────────────────
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }

    // ─────────────────────────────────────────────────────────────────────────────
    // dumpAll  — Print all diagnostics to the console with basic formatting
    //
    // Iterates through the raw diagnostic list and streams their text representations
    // to the specified output stream (defaults to standard error).
    // ─────────────────────────────────────────────────────────────────────────────
    void dumpAll(std::ostream& os = std::cerr) const;

private:
    std::vector<Diagnostic> diagnostics_; ///< Internal storage for all reports.
    int                     errorCount_   = 0; ///< Number of issues with severity >= Error.
    int                     warningCount_ = 0; ///< Number of Warning-severity issues.
};