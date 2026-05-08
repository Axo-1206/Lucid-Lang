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

    // Decides if this constitutes a failure or a warning.
    if (severity == DiagnosticSeverity::Error || severity == DiagnosticSeverity::Fatal) {
        errorCount_++;
    } else if (severity == DiagnosticSeverity::Warning) {
        warningCount_++;
    }
}

void DiagnosticEngine::error(DiagnosticCategory category, SourceLocation loc, DiagCode code, const std::string &msg) {
    // Convenience wrapper for standardized Error reporting.
    report(DiagnosticSeverity::Error, category, loc, code, msg);
}

void DiagnosticEngine::warning(DiagnosticCategory category, SourceLocation loc, DiagCode code, const std::string &msg) {
    // Convenience wrapper for standardized Warning reporting.
    report(DiagnosticSeverity::Warning, category, loc, code, msg);
}

void DiagnosticEngine::dumpAll(const StringPool& pool, std::ostream &os) const {
    for (const auto &d : diagnostics_) {
        // Determine prefix: W for warnings, E for everything else.
        // Warning codes are in the 5000+ range.
        char prefix = (d.severity == DiagnosticSeverity::Warning) ? 'W' : 'E';
        // Display warning codes as Wxxxx (relative to the 5000 base) so that
        // W3001 = code 5001 prints as "W3001", matching DiagnosticCodes.hpp docs.
        uint32_t displayCode = (d.severity == DiagnosticSeverity::Warning && d.code >= 5000)
            ? (d.code - 5000 + 3000)  // 5001 → 3001, 5002 → 3002, etc.
            : d.code;

        // Output format:
        // C:/path/file.luc:10:5 [E2001] Error: some message
        // C:/path/file.luc:10:5 [W3001] Warning: some message
        os << pool.lookup(d.location.file) << ":" << d.location.line << ":" << d.location.column
           << " [" << prefix << displayCode << "] "
           << severityToString(d.severity) << ": " << d.message << "\n";
    }
}