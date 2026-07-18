/**
 * @file Diagnostic.cpp
 * @brief Implementation of the procedural diagnostic interface.
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
// Global state – plain statics, not thread-local, because the compiler's
// pipeline is single-threaded and strictly sequential (parse fully finishes
// before analysis begins). See Diagnostic.hpp's "Single-threaded, on
// purpose" architecture note for the full reasoning and what a future
// multi-threaded compiler would need to change here.
// =============================================================================

static std::vector<Diagnostic> diagnostics;
static int errorCount = 0;
static int warningCount = 0;

// -----------------------------------------------------------------------------
// Source-scope stack — see Diagnostic.hpp's "source-scope stack"
// architectural note for the full rationale. Each frame remembers only an
// index into `diagnostics` (where it started) plus its own counters; the
// diagnostics themselves are never copied or moved anywhere.
// -----------------------------------------------------------------------------

namespace {
struct SourceFrame {
    InternedString file;
    size_t startIndex = 0;        // index into `diagnostics` at push time
    int errorCount = 0;           // Error reports since push/resetStreak
    int consecutiveErrors = 0;    // see resetStreak()'s doc comment
};
} // namespace

static std::vector<SourceFrame> sourceStack;

// =============================================================================
// Helper: Add a diagnostic to the global list, update whole-session
// counters, and update the innermost open source frame (if any).
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

    if (severity == DiagnosticSeverity::Error) {
        ++errorCount;
        if (!sourceStack.empty()) {
            SourceFrame& frame = sourceStack.back();
            ++frame.errorCount;
            ++frame.consecutiveErrors;
        }
    } else if (severity == DiagnosticSeverity::Warning) {
        ++warningCount;
        // Deliberately does NOT touch any frame's consecutiveErrors — this
        // is the fix for the bug that started this rewrite: a warning is
        // neither a failure nor a break in an error streak. See
        // Diagnostic.hpp's "What 'consecutive' means here" note.
    }
    // Note (severity == DiagnosticSeverity::Note) affects neither the
    // whole-session counters nor any frame's bookkeeping.
}

// =============================================================================
// Public reporting — explicit file
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
        DiagCode::E0001,  // Never actually consulted for its template — see
                          // dumpAll(): a Note's message is printed verbatim
                          // from `args[0]`, not looked up by code. This is
                          // just a placeholder value to satisfy the field.
        {msg}
    });
    // Notes do NOT affect error/warning counts or any frame's bookkeeping.
}

// =============================================================================
// Public reporting — implicit current source
// =============================================================================

void error(DiagnosticCategory category, SourceLocation loc, DiagCode code,
           std::initializer_list<std::string> args) {
    error(category, currentFile(), loc, code, args);
}

void warning(DiagnosticCategory category, SourceLocation loc, DiagCode code,
             std::initializer_list<std::string> args) {
    warning(category, currentFile(), loc, code, args);
}

void note(SourceLocation loc, const std::string& msg) {
    note(currentFile(), loc, msg);
}

// =============================================================================
// Whole-session queries
// =============================================================================

bool hasErrors() {
    return errorCount > 0;
}

bool hasWarnings() {
    return warningCount > 0;
}

int totalErrorCount() {
    return errorCount;
}

int totalWarningCount() {
    return warningCount;
}

DiagnosticSummary summarize() {
    DiagnosticSummary summary;
    summary.errorCount = errorCount;
    summary.warningCount = warningCount;

    std::unordered_set<InternedString> seenErrorFiles;
    std::unordered_set<InternedString> seenWarningFiles;

    for (const auto& d : diagnostics) {
        if (d.severity == DiagnosticSeverity::Error) {
            if (seenErrorFiles.insert(d.file).second) {
                summary.filesWithErrors.push_back(d.file);
            }
        } else if (d.severity == DiagnosticSeverity::Warning) {
            if (seenWarningFiles.insert(d.file).second) {
                summary.filesWithWarnings.push_back(d.file);
            }
        }
    }

    return summary;
}

void clear() {
    diagnostics.clear();
    errorCount = 0;
    warningCount = 0;
    // See clear()'s own doc comment in Diagnostic.hpp: forcibly cleared
    // rather than left holding now-invalid start indices, but calling this
    // out from under a live ScopedSource is a caller error this function
    // cannot detect.
    sourceStack.clear();
}

const std::vector<Diagnostic>& getAll() {
    return diagnostics;
}

std::vector<Diagnostic> getAllForFile(InternedString file) {
    std::vector<Diagnostic> result;
    for (const auto& d : diagnostics) {
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
    frame.startIndex = diagnostics.size();
    frame.errorCount = 0;
    frame.consecutiveErrors = 0;
    sourceStack.push_back(frame);
}

void popSource() {
    if (!sourceStack.empty()) {
        sourceStack.pop_back();
    }
}

InternedString currentFile() {
    return sourceStack.empty() ? InternedString{} : sourceStack.back().file;
}

bool hasErrorsInCurrentSource() {
    return !sourceStack.empty() && sourceStack.back().errorCount > 0;
}

int consecutiveErrorsInCurrentSource() {
    return sourceStack.empty() ? 0 : sourceStack.back().consecutiveErrors;
}

bool canContinue(int threshold) {
    return consecutiveErrorsInCurrentSource() < threshold;
}

void resetStreak() {
    if (!sourceStack.empty()) {
        sourceStack.back().consecutiveErrors = 0;
    }
}

std::vector<Diagnostic> currentSourceDiagnostics() {
    if (sourceStack.empty()) {
        return {};
    }
    const SourceFrame& frame = sourceStack.back();
    return std::vector<Diagnostic>(diagnostics.begin() + frame.startIndex,
                                    diagnostics.end());
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

// =============================================================================
// Helper: Human-readable severity label and code label for formatOneLine().
// =============================================================================

static const char* severityLabel(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Note:    return "NOTE";
        case DiagnosticSeverity::Warning: return "WARNING";
        case DiagnosticSeverity::Error:   return "ERROR";
    }
    return "UNKNOWN";
}

/**
 * @brief Build the "E1002"/"W0002"-style label shown after the severity.
 *
 * The displayed 4-digit number is the code's position within its own
 * range as named in DiagnosticCodes.hpp — NOT the raw `DiagCode` integer.
 * Every E-range in DiagnosticCodes.hpp is constructed so that
 * `symbolicNumber == rawValue + 1` (e.g. `E1001 = 1000` → symbolic 1001),
 * which holds uniformly across every E-range regardless of where that
 * range starts. The single exception is the Warning range, which starts
 * its own numbering back at `W0001` despite `W0001`'s raw value being
 * 6000 — so warnings need the one explicit offset below; nothing else
 * does. This replaces the previous version's incorrect assumption that
 * warnings lived at 5000+ (see dumpAll()'s git history / this file's own
 * earlier revision) with an offset actually derived from
 * DiagnosticCodes.hpp's real layout.
 */
static std::string codeLabel(const Diagnostic& diag) {
    uint32_t raw = static_cast<uint32_t>(diag.code);
    char prefix;
    uint32_t symbolic;

    if (diag.severity == DiagnosticSeverity::Warning) {
        prefix = 'W';
        symbolic = raw - 6000 + 1;
    } else {
        prefix = 'E';
        symbolic = raw + 1;
    }

    std::ostringstream oss;
    oss << prefix << std::setfill('0') << std::setw(4) << symbolic;
    return oss.str();
}

// =============================================================================
// Output
// =============================================================================

std::string formatOneLine(const Diagnostic& diag, const StringPool& pool) {
    std::ostringstream oss;

    oss << "[" << severityLabel(diag.severity) << "] ";

    // Notes never carry a real code (see note()'s own doc comment — the
    // DiagCode stored alongside one is a placeholder), so the "CODE: "
    // segment is only shown for Error/Warning.
    if (diag.severity != DiagnosticSeverity::Note) {
        oss << codeLabel(diag) << ": ";
    }

    // Message: Notes are raw text, everything else goes through the
    // templated formatter (see note()'s own doc comment for why).
    if (diag.severity == DiagnosticSeverity::Note) {
        oss << (diag.args.empty() ? std::string() : diag.args[0]);
    } else {
        oss << formatMessage(diag);
    }

    oss << "  " << pool.lookup(diag.file) << ":" << diag.location;

    return oss.str();
}

void dumpAll(const StringPool& pool, std::ostream& os) {
    for (const auto& diag : diagnostics) {
        os << formatOneLine(diag, pool) << "\n";
    }

    // Trailing summary line, in the spirit of rustc/tsc's "N errors
    // generated" — built from summarize() rather than re-tallying here, so
    // there's exactly one place that counts diagnostics.
    DiagnosticSummary summary = summarize();
    if (summary.errorCount > 0 || summary.warningCount > 0) {
        os << "\n" << summary.errorCount << " error(s), "
           << summary.warningCount << " warning(s) generated";
        if (!summary.filesWithErrors.empty() || !summary.filesWithWarnings.empty()) {
            std::unordered_set<InternedString> distinctFiles(
                summary.filesWithErrors.begin(), summary.filesWithErrors.end());
            distinctFiles.insert(summary.filesWithWarnings.begin(),
                                  summary.filesWithWarnings.end());
            os << " across " << distinctFiles.size() << " file(s)";
        }
        os << "\n";
    }
}

} // namespace diagnostic