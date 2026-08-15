/**
 * @file Diagnostic.hpp
 * @brief Unified diagnostic system - single header for all diagnostic functionality.
 *
 * @design_decision Simple and flexible
 *   - Error codes are unique identifiers with clear purposes
 *   - Messages are built at call site for maximum flexibility
 *   - Code range determines category and severity
 *   - Single context tracks all diagnostics
 *
 * @design_decision No code-to-message mapping
 *   - Messages are built directly at call site using variadic templates
 *   - This eliminates the need for template strings and message files
 *   - Makes error messages more readable and maintainable
 *
 * @design_decision Code ranges
 *   1000-1999: Lexical
 *   2000-2999: Syntax
 *   3000-3999: Semantic - Name Resolution
 *   4000-4999: Semantic - Type Checking
 *   5000-5999: Semantic - Generics/Traits/FFI
 *   6000-6999: Semantic - Other
 *   7000-7999: Backend
 *   8000-8999: Warnings (cross-cutting)
 */

#pragma once

#include "core/SourceLocation.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/DiagCode.hpp"

#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <ostream>

// ─── Severity and DiagCode ─────────────────────────────────────────────────
//
// Severity, DiagCode, and the pure helpers (categoryName, severityFromCode,
// isWarningCode, isErrorCode, severityName) now live in DiagCode.hpp
// (included above). They moved out of this file so that code needing just
// the code space - notably codegen/support/RuntimeError.hpp, which maps
// RuntimeErrorKind to a DiagCode so a runtime panic can embed the same code
// a compile-time diagnostic would use - doesn't have to pull in
// DiagnosticEngine's StringPool/BaseAST/<ostream> dependencies to get it.

// ─── Diagnostic ──────────────────────────────────────────────────────────

struct Diagnostic {
    Severity severity;
    DiagCode code;
    SourceLocation location;
    std::string message;

    std::string category() const {
        return categoryName(code);
    }
};

// ─── Diagnostic Engine ──────────────────────────────────────────────────

/// @brief Simple diagnostic engine - stores and tracks all diagnostics.
///
/// Usage:
///   DiagnosticEngine ctx;
///   ctx.error(ErrorCode::SemUndefinedValue, node, "undefined variable '", name, "'");
///   ctx.warning(ErrorCode::WarnUnusedVariable, node, "unused variable '", name, "'");
///   ctx.note(node, "consider using '_' to ignore");
///
///   if (ctx.canContinue()) { ... }
///   ctx.dump(std::cerr);
class DiagnosticEngine {
public:
    // ─── Report Functions ──────────────────────────────────────────────

    /// Report an error with a diagnostic code.
    template<typename... Args>
    void error(DiagCode code, BaseAST* node, Args&&... args) {
        add(severityFromCode(code), code,
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a warning with a diagnostic code.
    template<typename... Args>
    void warning(DiagCode code, BaseAST* node, Args&&... args) {
        add(Severity::Warning, code,
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a free-text note.
    template<typename... Args>
    void note(BaseAST* node, Args&&... args) {
        add(Severity::Note, DiagCode(0),
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a free-text hint.
    template<typename... Args>
    void hint(BaseAST* node, Args&&... args) {
        add(Severity::Hint, DiagCode(0),
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    // ─── Convenience overloads with explicit location ─────────────────

    template<typename... Args>
    void errorAt(DiagCode code, const SourceLocation& loc, Args&&... args) {
        add(severityFromCode(code), code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template<typename... Args>
    void warningAt(DiagCode code, const SourceLocation& loc, Args&&... args) {
        add(Severity::Warning, code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template<typename... Args>
    void noteAt(const SourceLocation& loc, Args&&... args) {
        add(Severity::Note, DiagCode(0), loc,
            buildMessage(std::forward<Args>(args)...));
    }

    // ─── Query Functions ───────────────────────────────────────────────

    /// Check if there are any errors.
    bool hasErrors() const {
        for (const auto& d : m_diagnostics) {
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) {
                return true;
            }
        }
        return false;
    }

    /// Check if there are any warnings.
    bool hasWarnings() const {
        for (const auto& d : m_diagnostics) {
            if (d.severity == Severity::Warning) {
                return true;
            }
        }
        return false;
    }

    /// Get the total number of errors.
    int errorCount() const {
        int count = 0;
        for (const auto& d : m_diagnostics) {
            if (d.severity == Severity::Error || d.severity == Severity::Fatal) {
                ++count;
            }
        }
        return count;
    }

    /// Get the total number of warnings.
    int warningCount() const {
        int count = 0;
        for (const auto& d : m_diagnostics) {
            if (d.severity == Severity::Warning) {
                ++count;
            }
        }
        return count;
    }

    /// Get the total number of all diagnostics.
    int totalCount() const {
        return static_cast<int>(m_diagnostics.size());
    }

    /// Check if we can continue (error count < maxErrors).
    bool canContinue(int maxErrors = 100) const {
        return errorCount() < maxErrors;
    }

    /// Get all diagnostics.
    const std::vector<Diagnostic>& all() const { return m_diagnostics; }

    /// Clear all diagnostics.
    void clear() { m_diagnostics.clear(); }

    // ─── Formatting ────────────────────────────────────────────────────

    /// Dump all diagnostics to an output stream.
    void dump(std::ostream& os = std::cerr) const {
        for (const auto& d : m_diagnostics) {
            os << formatOneLine(d) << "\n";
        }
        if (hasErrors() || hasWarnings()) {
            os << "\n" << errorCount() << " error(s), "
               << warningCount() << " warning(s)\n";
        }
    }

    /// Format a single diagnostic as one line.
    std::string formatOneLine(const Diagnostic& d) const {
        std::ostringstream oss;
        oss << "[" << severityName(d.severity) << "] ";

        // Format code if it's not a free-text note/hint
        if (d.code != DiagCode(0)) {
            uint32_t raw = static_cast<uint32_t>(d.code);
            char prefix = isWarningCode(d.code) ? 'W' : 'E';
            oss << prefix << std::setfill('0') << std::setw(4) << raw << ": ";
        }

        oss << d.message;

        if (d.location.isKnown()) {
            oss << " at " << d.location.line() << ":" << d.location.column();
        }

        return oss.str();
    }

    /// Format a diagnostic with ANSI colors.
    std::string formatOneLineWithColor(const Diagnostic& d) const {
        const char* reset = "\033[0m";
        const char* color;

        switch (d.severity) {
            case Severity::Fatal:
            case Severity::Error:   color = "\033[31m"; break;  // Red
            case Severity::Warning: color = "\033[33m"; break;  // Yellow
            case Severity::Note:    color = "\033[36m"; break;  // Cyan
            case Severity::Hint:    color = "\033[90m"; break;  // Gray
            default:                color = reset; break;
        }

        return std::string(color) + formatOneLine(d) + reset;
    }

    /// Dump with colors.
    void dumpWithColor(std::ostream& os = std::cerr) const {
        for (const auto& d : m_diagnostics) {
            os << formatOneLineWithColor(d) << "\n";
        }
        if (hasErrors() || hasWarnings()) {
            const char* color = hasErrors() ? "\033[31m" : "\033[33m";
            const char* reset = "\033[0m";
            os << "\n" << color << errorCount() << " error(s), "
               << warningCount() << " warning(s)" << reset << "\n";
        }
    }

private:
    std::vector<Diagnostic> m_diagnostics;

    void add(Severity sev, DiagCode code, const SourceLocation& loc, std::string msg) {
        m_diagnostics.push_back({sev, code, loc, std::move(msg)});
    }

    // ─── Message Building ──────────────────────────────────────────────

    template<typename T>
    std::string toString(const T& value) {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    std::string toString(InternedString s) {
        return StringPool::instance().lookup(s);
    }

    std::string toString(const char* s) {
        return std::string(s);
    }

    std::string toString(const std::string& s) {
        return s;
    }

    std::string toString(bool b) {
        return b ? "true" : "false";
    }

    std::string toString(int v) {
        return std::to_string(v);
    }

    std::string toString(size_t v) {
        return std::to_string(v);
    }

    std::string toString(double v) {
        return std::to_string(v);
    }

    template<typename T>
    std::string toString(const T* ptr) {
        if (!ptr) return "null";
        std::ostringstream oss;
        oss << ptr;
        return oss.str();
    }

    template<typename T, typename... Rest>
    std::string buildMessage(const T& first, Rest&&... rest) {
        return toString(first) + buildMessage(std::forward<Rest>(rest)...);
    }

    std::string buildMessage() {
        return "";
    }
};