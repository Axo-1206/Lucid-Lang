/**
 * @file Diagnostic.cpp
 * @brief Non-template implementation of DiagnosticEngine.
 *
 * ─── Why this file is not header-only ─────────────────────────────────────
 * Three reasons:
 *
 *   1. `locationOf(BaseAST*)` dereferences the AST. It has to include the
 *      AST header, and the AST header is large. Keeping this include out
 *      of Diagnostic.hpp is what lets every frontend translation unit
 *      include the diagnostic header without paying the AST parse cost.
 *
 *   2. `toString(InternedString)` reaches into StringPool. Same reasoning:
 *      StringPool.hpp is heavy and only this file needs it.
 *
 *   3. `dump` and `formatOneLine` are non-trivial. Putting them in the
 *      header would inline them into every caller, bloating the code and
 *      the compile time.
 */

#include "Diagnostic.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/memory/StringPool.hpp"

#include <iomanip>

namespace lucid::diag {

// ─────────────────────────────────────────────────────────────────────────────
// locationOf
// ─────────────────────────────────────────────────────────────────────────────

SourceLocation locationOf(const BaseAST* node) noexcept {
    return node ? node->loc : SourceLocation{};
}

// ─────────────────────────────────────────────────────────────────────────────
// toString (the one overload that needs the pool)
// ─────────────────────────────────────────────────────────────────────────────

std::string DiagnosticEngine::toString(InternedString s) const {
    if (!s.isValid()) return {};
    if (!m_pool) {
        // No pool: format the raw ID so the diagnostic is still readable.
        // This happens for driver-level errors raised before a compilation
        // session sets up its pool.
        return "<interned:" + std::to_string(s.id) + ">";
    }
    return m_pool->lookup(s);
}

// ─────────────────────────────────────────────────────────────────────────────
// Query
// ─────────────────────────────────────────────────────────────────────────────

bool DiagnosticEngine::hasErrors() const noexcept {
    for (const auto& d : m_diagnostics) {
        if (d.isError()) return true;
    }
    return false;
}

bool DiagnosticEngine::hasWarnings() const noexcept {
    for (const auto& d : m_diagnostics) {
        if (d.isWarning()) return true;
    }
    return false;
}

int DiagnosticEngine::errorCount() const noexcept {
    int count = 0;
    for (const auto& d : m_diagnostics) {
        if (d.isError()) ++count;
    }
    return count;
}

int DiagnosticEngine::warningCount() const noexcept {
    int count = 0;
    for (const auto& d : m_diagnostics) {
        if (d.isWarning()) ++count;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Formatting
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The single-character prefix a code is rendered with.
/// Returns '\0' for free-text diagnostics (code 0), which render no prefix.
char codePrefixFor(Severity sev, DiagCode code) noexcept {
    if (raw(code) == 0) return '\0';
    switch (sev) {
        case Severity::Fatal:
        case Severity::Error:   return 'E';
        case Severity::Warning: return 'W';
        case Severity::Note:    return 'N';
        case Severity::Hint:    return 'H';
    }
    return '?';
}

/// ANSI color for a severity. Empty string for no color.
const char* colorFor(Severity sev) noexcept {
    switch (sev) {
        case Severity::Fatal:   return "\033[1;31m";  // bright red
        case Severity::Error:   return "\033[31m";    // red
        case Severity::Warning: return "\033[33m";    // yellow
        case Severity::Note:    return "\033[36m";    // cyan
        case Severity::Hint:    return "\033[90m";    // gray
    }
    return "";
}

constexpr const char* kReset = "\033[0m";

} // namespace

std::string DiagnosticEngine::formatOneLine(const Diagnostic& d) const {
    std::ostringstream oss;

    oss << "[" << severityName(d.severity) << "] ";

    const char prefix = codePrefixFor(d.severity, d.code);
    if (prefix != '\0') {
        oss << prefix
            << std::setfill('0') << std::setw(4) << raw(d.code)
            << ": ";
    }

    oss << d.message;

    if (d.location.isKnown()) {
        oss << " at " << d.location.line() << ":" << d.location.column();
    }

    if (d.file.isValid() && m_pool) {
        oss << " in " << m_pool->lookupView(d.file);
    }

    return oss.str();
}

std::string DiagnosticEngine::formatOneLineWithColor(const Diagnostic& d) const {
    const char* color = colorFor(d.severity);
    return std::string(color) + formatOneLine(d) + kReset;
}

void DiagnosticEngine::dump(std::ostream& os) const {
    for (const auto& d : m_diagnostics) {
        os << formatOneLine(d) << '\n';
    }
    if (!m_diagnostics.empty()) {
        os << '\n'
           << errorCount()   << " error(s), "
           << warningCount() << " warning(s)\n";
    }
}

void DiagnosticEngine::dumpWithColor(std::ostream& os) const {
    for (const auto& d : m_diagnostics) {
        os << formatOneLineWithColor(d) << '\n';
    }
    if (!m_diagnostics.empty()) {
        const char* summaryColor = hasErrors() ? colorFor(Severity::Error)
                                               : colorFor(Severity::Warning);
        os << '\n'
           << summaryColor
           << errorCount()   << " error(s), "
           << warningCount() << " warning(s)"
           << kReset << '\n';
    }
}

} // namespace lucid::diag