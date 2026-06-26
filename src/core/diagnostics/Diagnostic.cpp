/**
 * @file Diagnostic.cpp
 * @brief Implementation of the procedural diagnostic interface.
 */

#include "Diagnostic.hpp"
#include "DiagnosticMessages.hpp"
#include "../memory/StringPool.hpp"
#include <iostream>

namespace diagnostic {

// =============================================================================
// Global state – thread‑local because compilation is sequential, but we keep
// it as plain static globals for simplicity. In a multi‑threaded compiler,
// this would need to be thread‑local or passed explicitly.
// =============================================================================

static std::vector<Diagnostic> diagnostics;
static int errorCount = 0;
static int warningCount = 0;

// =============================================================================
// Helper: Add a diagnostic to the global list and update counters.
// =============================================================================

static void add(DiagnosticSeverity severity, DiagnosticCategory category,
                InternedString file, SourceLocation loc, DiagCode code,
                std::initializer_list<std::string> args) {
    diagnostics.push_back({
        severity,
        category,
        file,
        loc,
        code,
        std::vector<std::string>(args.begin(), args.end())
    });

    if (severity == DiagnosticSeverity::Error || severity == DiagnosticSeverity::Fatal) {
        ++errorCount;
    } else if (severity == DiagnosticSeverity::Warning) {
        ++warningCount;
    }
}

// =============================================================================
// Public reporting functions
// =============================================================================

void error(DiagnosticCategory category, InternedString file,
           SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args) {
    add(DiagnosticSeverity::Error, category, file, loc, code, args);
}

void warning(DiagnosticCategory category, InternedString file,
             SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args) {
    add(DiagnosticSeverity::Warning, category, file, loc, code, args);
}

void note(InternedString file, SourceLocation loc, const std::string& msg) {
    diagnostics.push_back({
        DiagnosticSeverity::Note,
        DiagnosticCategory::General,
        file,
        loc,
        DiagCode::E0001,  // generic "file not found" – but we repurpose for notes
        {msg}
    });
    // Notes do NOT affect error/warning counts.
}

// =============================================================================
// Query functions
// =============================================================================

bool hasErrors() {
    return errorCount > 0;
}

bool hasWarnings() {
    return warningCount > 0;
}

void clear() {
    diagnostics.clear();
    errorCount = 0;
    warningCount = 0;
}

const std::vector<Diagnostic>& getAll() {
    return diagnostics;
}

// =============================================================================
// Helper: Format a single diagnostic's message with its arguments.
// =============================================================================

static std::string formatMessage(const Diagnostic& diag) {
    std::string_view tmpl = diagnosticMessages::getMessage(diag.code);
    if (diag.args.empty()) {
        return std::string(tmpl);
    }

    std::string result;
    size_t argIdx = 0;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '%' && i + 1 < tmpl.size() && tmpl[i + 1] == 's') {
            if (argIdx < diag.args.size()) {
                result += diag.args[argIdx++];
            } else {
                result += "<?>";
            }
            ++i; // skip the 's'
        } else {
            result += tmpl[i];
        }
    }
    return result;
}

// =============================================================================
// Output: Print all diagnostics to the given stream.
// =============================================================================

void dumpAll(const StringPool& pool, std::ostream& os) {
    for (const auto& diag : diagnostics) {
        // Determine prefix and display code
        char prefix;
        uint32_t displayCode = static_cast<uint32_t>(diag.code);

        if (diag.severity == DiagnosticSeverity::Warning) {
            prefix = 'W';
            // Warning codes are stored in the 5000+ range.
            // Display them as W3xxx where W3001 = 5001, etc.
            if (displayCode >= 5000) {
                displayCode = displayCode - 5000 + 3000;
            }
        } else if (diag.severity == DiagnosticSeverity::Note) {
            prefix = 'N';
            displayCode = 0;  // notes have no code
        } else {
            prefix = 'E';
            // Error codes are displayed as‑is (e.g., E2001).
        }

        // Print location
        os << pool.lookup(diag.file) << ":" << diag.location.line()
           << ":" << diag.location.column();

        if (displayCode != 0) {
            os << " [" << prefix << displayCode << "]";
        } else {
            os << " [" << prefix << "]";
        }

        os << " ";

        // Print severity
        switch (diag.severity) {
            case DiagnosticSeverity::Note:    os << "Note: "; break;
            case DiagnosticSeverity::Warning: os << "Warning: "; break;
            case DiagnosticSeverity::Error:   os << "Error: "; break;
            case DiagnosticSeverity::Fatal:   os << "Fatal: "; break;
        }

        // Print formatted message
        os << formatMessage(diag) << "\n";
    }
}

} // namespace diagnostic