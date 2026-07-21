/**
 * @file Diagnostic.cpp
 * @brief Implementation of the procedural diagnostic interface.
 *
 * This is the SINGLE implementation file for all diagnostic functionality.
 * It combines:
 *   - Global state management
 *   - Diagnostic data types (Diagnostic, Summary)
 *   - Reporting API (error, warning, note, hint)
 *   - Source scope management (ScopedSource, pushSource, etc.)
 *   - Query API (hasErrors, totalErrorCount, summarize, etc.)
 *   - Formatting API (formatOneLine, dumpAll, etc.)
 */

#include "Diagnostic.hpp"
#include "DiagnosticMessages.hpp"
#include "../memory/StringPool.hpp"

#include <iostream>
#include <unordered_set>
#include <sstream>
#include <iomanip>

namespace diagnostic {

// =============================================================================
// Helper: Category from Code Range
// =============================================================================

std::string_view Diagnostic::category() const {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw < 100) return "Environment";
    if (raw < 1000) return "Lexical";
    if (raw < 2000) return "Syntax";
    if (raw < 3000) return "Semantic/NameResolution";
    if (raw < 4000) return "Semantic/TypeChecking";
    if (raw < 5000) return "Semantic/GenericsTraitsFFI";
    if (raw < 6000) return "Backend";
    if (raw < 7000) return "Warning";
    return "Unknown";
}

// =============================================================================
// Helper: Summary
// =============================================================================

size_t Summary::totalFilesWithDiagnostics() const {
    std::unordered_set<InternedString> all;
    all.insert(filesWithErrors.begin(), filesWithErrors.end());
    all.insert(filesWithWarnings.begin(), filesWithWarnings.end());
    return all.size();
}

// =============================================================================
// Helper: Format a diagnostic's message with its arguments.
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
// Helper: Code label (E1002, W0002, etc.)
// =============================================================================

static std::string codeLabel(const Diagnostic& diag) {
    uint32_t raw = static_cast<uint32_t>(diag.code);
    char prefix;
    uint32_t symbolic;

    if (diag.severity == Severity::Warning) {
        prefix = 'W';
        symbolic = raw - 6000;
    } else {
        prefix = 'E';
        symbolic = raw;
    }

    std::ostringstream oss;
    oss << prefix << std::setfill('0') << std::setw(4) << symbolic;
    return oss.str();
}

// =============================================================================
// Helper: Get StringPool instance
// =============================================================================

static StringPool& pool() {
    return StringPool::instance();
}

// =============================================================================
// Global state – plain statics, not thread-local
// =============================================================================

static std::vector<Diagnostic> g_diagnostics;
static int g_errorCount = 0;
static int g_warningCount = 0;
static int g_noteCount = 0;
static int g_hintCount = 0;

// ─── Source-scope stack ────────────────────────────────────────────────────

namespace {
struct SourceFrame {
    InternedString file;
    size_t startIndex = 0;
    int errorCount = 0;
    int warningCount = 0;
    int noteCount = 0;
    int hintCount = 0;
    int consecutiveErrors = 0;
};
} // namespace

static std::vector<SourceFrame> g_sourceStack;

// =============================================================================
// Helper: Add a diagnostic
// =============================================================================

static void add(Severity severity, InternedString file,
                SourceLocation loc, DiagCode code,
                std::initializer_list<std::string> args) {
    g_diagnostics.push_back({
        severity,
        file,
        loc,
        code,
        std::vector<std::string>(args.begin(), args.end())
    });

    // Update session counters
    switch (severity) {
        case Severity::Error:
        case Severity::Fatal:
            ++g_errorCount;
            break;
        case Severity::Warning:
            ++g_warningCount;
            break;
        case Severity::Note:
            ++g_noteCount;
            break;
        case Severity::Hint:
            ++g_hintCount;
            break;
    }

    // Update current source frame
    if (!g_sourceStack.empty()) {
        SourceFrame& frame = g_sourceStack.back();
        switch (severity) {
            case Severity::Error:
            case Severity::Fatal:
                ++frame.errorCount;
                ++frame.consecutiveErrors;
                break;
            case Severity::Warning:
                ++frame.warningCount;
                break;
            case Severity::Note:
                ++frame.noteCount;
                break;
            case Severity::Hint:
                ++frame.hintCount;
                break;
        }
    }
}

// =============================================================================
// Public reporting — explicit file
// =============================================================================

void error(InternedString file, SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args) {
    add(Severity::Error, file, loc, code, args);
}

void warning(InternedString file, SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args) {
    add(Severity::Warning, file, loc, code, args);
}

void note(InternedString file, SourceLocation loc, const std::string& msg) {
    add(Severity::Note, file, loc, DiagCode::E0001, {msg});
}

void hint(InternedString file, SourceLocation loc, const std::string& msg) {
    add(Severity::Hint, file, loc, DiagCode::E0001, {msg});
}

// =============================================================================
// Public reporting — implicit current source
// =============================================================================

void error(SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args) {
    error(currentFile(), loc, code, args);
}

void warning(SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args) {
    warning(currentFile(), loc, code, args);
}

void note(SourceLocation loc, const std::string& msg) {
    note(currentFile(), loc, msg);
}

void hint(SourceLocation loc, const std::string& msg) {
    hint(currentFile(), loc, msg);
}

// =============================================================================
// Whole-session queries
// =============================================================================

bool hasErrors() {
    return g_errorCount > 0;
}

bool hasWarnings() {
    return g_warningCount > 0;
}

int totalErrorCount() {
    return g_errorCount;
}

int totalWarningCount() {
    return g_warningCount;
}

int totalNoteCount() {
    return g_noteCount;
}

int totalHintCount() {
    return g_hintCount;
}

Summary summarize() {
    Summary summary;
    summary.errorCount = g_errorCount;
    summary.warningCount = g_warningCount;
    summary.noteCount = g_noteCount;
    summary.hintCount = g_hintCount;

    std::unordered_set<InternedString> seenErrors;
    std::unordered_set<InternedString> seenWarnings;

    for (const auto& d : g_diagnostics) {
        if (d.severity == Severity::Error || d.severity == Severity::Fatal) {
            if (seenErrors.insert(d.file).second) {
                summary.filesWithErrors.push_back(d.file);
            }
        } else if (d.severity == Severity::Warning) {
            if (seenWarnings.insert(d.file).second) {
                summary.filesWithWarnings.push_back(d.file);
            }
        }
    }

    return summary;
}

void clear() {
    g_diagnostics.clear();
    g_errorCount = 0;
    g_warningCount = 0;
    g_noteCount = 0;
    g_hintCount = 0;
    g_sourceStack.clear();
}

const std::vector<Diagnostic>& getAll() {
    return g_diagnostics;
}

std::vector<Diagnostic> getAllForFile(InternedString file) {
    std::vector<Diagnostic> result;
    for (const auto& d : g_diagnostics) {
        if (d.file == file) {
            result.push_back(d);
        }
    }
    return result;
}

// =============================================================================
// Source scope
// =============================================================================

void pushSource(InternedString file) {
    SourceFrame frame;
    frame.file = file;
    frame.startIndex = g_diagnostics.size();
    frame.errorCount = 0;
    frame.warningCount = 0;
    frame.noteCount = 0;
    frame.hintCount = 0;
    frame.consecutiveErrors = 0;
    g_sourceStack.push_back(frame);
}

void popSource() {
    if (!g_sourceStack.empty()) {
        g_sourceStack.pop_back();
    }
}

InternedString currentFile() {
    return g_sourceStack.empty() ? InternedString{} : g_sourceStack.back().file;
}

bool hasErrorsInCurrentSource() {
    return !g_sourceStack.empty() && g_sourceStack.back().errorCount > 0;
}

int consecutiveErrorsInCurrentSource() {
    return g_sourceStack.empty() ? 0 : g_sourceStack.back().consecutiveErrors;
}

bool canContinue(int threshold) {
    return consecutiveErrorsInCurrentSource() < threshold;
}

void resetStreak() {
    if (!g_sourceStack.empty()) {
        g_sourceStack.back().consecutiveErrors = 0;
    }
}

std::vector<Diagnostic> currentSourceDiagnostics() {
    if (g_sourceStack.empty()) {
        return {};
    }
    const SourceFrame& frame = g_sourceStack.back();
    return std::vector<Diagnostic>(
        g_diagnostics.begin() + static_cast<ptrdiff_t>(frame.startIndex),
        g_diagnostics.end()
    );
}

// =============================================================================
// ScopedSource
// =============================================================================

ScopedSource::ScopedSource(InternedString file) : m_file(file) {
    pushSource(file);
}

ScopedSource::~ScopedSource() {
    popSource();
}

// =============================================================================
// Formatting
// =============================================================================

std::string formatOneLine(const Diagnostic& diag) {
    std::ostringstream oss;

    oss << "[" << severityName(diag.severity) << "] ";

    // Notes and hints don't carry a real code
    if (diag.severity != Severity::Note && diag.severity != Severity::Hint) {
        oss << codeLabel(diag) << ": ";
    }

    // Format the message
    if (diag.severity == Severity::Note || diag.severity == Severity::Hint) {
        oss << (diag.args.empty() ? std::string() : diag.args[0]);
    } else {
        oss << formatMessage(diag);
    }

    oss << "  " << pool().lookup(diag.file) << ":" << diag.location;

    return oss.str();
}

std::string formatOneLineWithColor(const Diagnostic& diag) {
    const char* reset = "\033[0m";
    const char* color;

    switch (diag.severity) {
        case Severity::Fatal:
        case Severity::Error:   color = "\033[31m"; break;  // Red
        case Severity::Warning: color = "\033[33m"; break;  // Yellow
        case Severity::Note:    color = "\033[36m"; break;  // Cyan
        case Severity::Hint:    color = "\033[90m"; break;  // Gray
        default:                color = reset; break;
    }

    std::string plain = formatOneLine(diag);
    return std::string(color) + plain + reset;
}

std::string formatJSON(const Diagnostic& diag) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"severity\":\"" << severityName(diag.severity) << "\",";
    oss << "\"severityLevel\":" << static_cast<int>(diag.severity) << ",";
    oss << "\"code\":\"" << codeLabel(diag) << "\",";
    oss << "\"message\":\"" << formatMessage(diag) << "\",";
    oss << "\"file\":\"" << pool().lookup(diag.file) << "\",";
    oss << "\"line\":" << diag.location.line() << ",";
    oss << "\"column\":" << diag.location.column();
    oss << "}";
    return oss.str();
}

void dumpAll(std::ostream& os) {
    for (const auto& diag : g_diagnostics) {
        os << formatOneLine(diag) << "\n";
    }

    Summary summary = summarize();
    if (summary.errorCount > 0 || summary.warningCount > 0) {
        os << "\n" << summary.errorCount << " error(s), "
           << summary.warningCount << " warning(s) generated";
        size_t totalFiles = summary.totalFilesWithDiagnostics();
        if (totalFiles > 0) {
            os << " across " << totalFiles << " file(s)";
        }
        os << "\n";
    }
}

void dumpAllWithColor(std::ostream& os) {
    for (const auto& diag : g_diagnostics) {
        os << formatOneLineWithColor(diag) << "\n";
    }

    Summary summary = summarize();
    if (summary.errorCount > 0 || summary.warningCount > 0) {
        const char* color = summary.errorCount > 0 ? "\033[31m" : "\033[33m";
        const char* reset = "\033[0m";
        os << "\n" << color << summary.errorCount << " error(s), "
           << summary.warningCount << " warning(s) generated" << reset;
        size_t totalFiles = summary.totalFilesWithDiagnostics();
        if (totalFiles > 0) {
            os << " across " << totalFiles << " file(s)";
        }
        os << "\n";
    }
}

} // namespace diagnostic