/**
 * @file DiagnosticEngine.cpp
 * 
 * @responsibility Implementation of the DiagnosticEngine methods.
 * 
 * @logic Collects Diagnostic objects and prints them with simple formatting.
 */

#include "DiagnosticEngine.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// severityToString  — Helper to convert severity to its human-readable name
//
// Maps the numeric DiagnosticSeverity enum into string tags like "Note", "Warning",
// "Error", or "Fatal" for use in reporting output.
// ─────────────────────────────────────────────────────────────────────────────
static std::string severityToString(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Note:    return "Note";
        case DiagnosticSeverity::Warning: return "Warning";
        case DiagnosticSeverity::Error:   return "Error";
        case DiagnosticSeverity::Fatal:   return "Fatal";
    }
    return "Unknown";
}

void DiagnosticEngine::report(DiagnosticSeverity severity, DiagnosticCategory category,
                              SourceLocation loc, DiagCode code, const std::string &msg) {
    // Assemble the diagnostic and push to the list.
    diagnostics_.push_back({severity, category, loc, static_cast<uint32_t>(code), msg});

    // Decides if this constitutes a failure.
    if (severity == DiagnosticSeverity::Error || severity == DiagnosticSeverity::Fatal) {
        errorCount_++;
    }
}

void DiagnosticEngine::error(DiagnosticCategory category, SourceLocation loc, DiagCode code, const std::string &msg) {
    // Convenience wrapper for standardized Error reporting.
    report(DiagnosticSeverity::Error, category, loc, code, msg);
}

void DiagnosticEngine::dumpAll(std::ostream &os) const {
    for (const auto &d : diagnostics_) {
        // Output format:
        // C:/path/file.luc:10:5 [E2001] Error: some message
        os << d.location.file << ":" << d.location.line << ":" << d.location.column
           << " [E" << d.code << "] " << severityToString(d.severity) << ": " << d.message << "\n";
    }
}
