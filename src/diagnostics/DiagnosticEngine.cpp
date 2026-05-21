/**
 * @file DiagnosticEngine.cpp
 * 
 * @responsibility Implementation of the DiagnosticEngine methods.
 * 
 * @logic Collects Diagnostic objects and prints them with simple formatting.
 */

#include "DiagnosticEngine.hpp"
#include "DiagnosticMessages.hpp"
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// severityToString  — Helper to convert severity to its human-readable name
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
                              InternedString file, SourceLocation loc, DiagCode code,
                              std::initializer_list<std::string> args) {
    // Assemble the diagnostic and push to the list.
    diagnostics_.push_back({severity, category, file, loc, code, std::vector<std::string>(args)});

    // Track counts for quick access
    if (severity == DiagnosticSeverity::Error || severity == DiagnosticSeverity::Fatal) {
        errorCount_++;
    } else if (severity == DiagnosticSeverity::Warning) {
        warningCount_++;
    }
}

void DiagnosticEngine::error(DiagnosticCategory category, InternedString file,
                             SourceLocation loc, DiagCode code,
                             std::initializer_list<std::string> args) {
    report(DiagnosticSeverity::Error, category, file, loc, code, args);
}

void DiagnosticEngine::warning(DiagnosticCategory category, InternedString file,
                               SourceLocation loc, DiagCode code,
                               std::initializer_list<std::string> args) {
    report(DiagnosticSeverity::Warning, category, file, loc, code, args);
}

void DiagnosticEngine::note(InternedString file, SourceLocation loc, const std::string& msg) {
    diagnostics_.push_back({DiagnosticSeverity::Note, DiagnosticCategory::General,
                            file, loc, DiagCode::E0001, {msg}});
    // Notes don't affect error/warning counts
}

void DiagnosticEngine::clear() {
    diagnostics_.clear();
    errorCount_ = 0;
    warningCount_ = 0;
}

std::string DiagnosticEngine::formatMessage(const Diagnostic& diag) const {
    std::string_view templateMsg = DiagnosticMessages::getMessage(diag.code);
    if (diag.args.empty()) {
        return std::string(templateMsg);
    }

    // Format: replace %s with arguments in order
    std::string result;
    size_t argIdx = 0;
    for (size_t i = 0; i < templateMsg.size(); ++i) {
        if (templateMsg[i] == '%' && i + 1 < templateMsg.size() && templateMsg[i + 1] == 's') {
            if (argIdx < diag.args.size()) {
                result += diag.args[argIdx++];
            } else {
                result += "<?>";
            }
            ++i; // skip the 's'
        } else {
            result += templateMsg[i];
        }
    }
    return result;
}

void DiagnosticEngine::dumpAll(const StringPool& pool, std::ostream& os) const {
    for (const auto& d : diagnostics_) {
        // Determine prefix: W for warnings, N for notes, E for everything else.
        char prefix;
        uint32_t displayCode;
        
        switch (d.severity) {
            case DiagnosticSeverity::Warning:
                prefix = 'W';
                // Warning codes are in the 5000+ range. Display as Wxxxx where
                // W3001 = code 5001, W3002 = 5002, etc.
                displayCode = (static_cast<uint32_t>(d.code) >= 5000)
                    ? (static_cast<uint32_t>(d.code) - 5000 + 3000)
                    : static_cast<uint32_t>(d.code);
                break;
            case DiagnosticSeverity::Note:
                prefix = 'N';
                displayCode = 0;
                break;
            default:
                prefix = 'E';
                displayCode = static_cast<uint32_t>(d.code);
                break;
        }

        // Format: C:/path/file.luc:10:5 [E2001] Error: some message
        os << pool.lookup(d.file) << ":" << d.location.line() << ":" << d.location.column();
        
        if (displayCode != 0) {
            os << " [" << prefix << displayCode << "]";
        } else {
            os << " [" << prefix << "]";
        }
        
        os << " " << severityToString(d.severity) << ": " << formatMessage(d) << "\n";
    }
}