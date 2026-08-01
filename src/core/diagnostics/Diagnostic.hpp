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

#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <ostream>

namespace diagnostic {

// ─── Severity ──────────────────────────────────────────────────────────────

enum class Severity : uint8_t {
    Hint    = 0,
    Note    = 1,
    Warning = 2,
    Error   = 3,
    Fatal   = 4,
};

inline const char* severityName(Severity s) {
    switch (s) {
        case Severity::Hint:    return "HINT";
        case Severity::Note:    return "NOTE";
        case Severity::Warning: return "WARNING";
        case Severity::Error:   return "ERROR";
        case Severity::Fatal:   return "FATAL";
    }
    return "UNKNOWN";
}

// ─── Error Codes ──────────────────────────────────────────────────────────

enum class ErrorCode : uint32_t {
    // ─── Lexical (1000-1999) ────────────────────────────────────────────
    LexInvalidCharacter = 1001,
    LexUnterminatedString = 1002,
    LexUnterminatedRawString = 1003,
    LexUnterminatedBlockComment = 1004,
    LexUnknownCharacter = 1005,

    // ─── Syntax (2000-2999) ─────────────────────────────────────────────
    SyntaxExpectedIdentifier = 2001,
    SyntaxExpectedType = 2002,
    SyntaxExpectedToken = 2003,
    SyntaxUnexpectedToken = 2004,
    SyntaxExpectedExpression = 2005,
    SyntaxExpectedBlock = 2006,
    SyntaxMultipleDefaults = 2007,
    SyntaxEmptyGroup = 2008,
    SyntaxExpectedPipelineSeed = 2009,
    SyntaxExpectedModulePath = 2010,
    SyntaxExpectedAttributeLiteral = 2011,
    SyntaxExpectedSwitchSubject = 2012,

    // ─── Semantic - Name Resolution (3000-3999) ────────────────────────
    SemUndefinedValue = 3001,
    SemUndefinedType = 3002,
    SemNotCallable = 3003,
    SemRedeclaration = 3004,
    SemUndefinedModule = 3005,
    SemUndefinedMember = 3006,
    SemGenericParamUnused = 3007,
    SemTraitNotFound = 3008,
    SemNotATrait = 3009,
    SemFieldNotFound = 3010,
    SemGenericParamRedeclaration = 3011,

    // ─── Semantic - Type Checking (4000-4999) ──────────────────────────
    SemTypeMismatch = 4001,
    SemArgCountMismatch = 4002,
    SemMissingInitializer = 4003,
    SemConstNullable = 4004,
    SemMissingReturn = 4005,
    SemDuplicateValue = 4006,
    SemUnknownIntrinsic = 4007,
    SemSelfReference = 4008,
    SemInvalidSwitchType = 4009,
    SemMissingCase = 4010,
    SemPipelineMismatch = 4011,
    SemCompositionMismatch = 4012,
    SemRefInStruct = 4013,
    SemIllegalNilErr = 4014,
    SemInvalidGenericArg = 4015,

    // ─── Semantic - Generics/Traits/FFI (5000-5999) ────────────────────
    SemGenericArityMismatch = 5001,
    SemGenericConstraint = 5002,
    SemTraitImplementation = 5003,
    SemTraitConflict = 5004,
    SemForeignInvalid = 5005,
    SemForeignABI = 5006,
    SemAttributeInvalid = 5007,
    SemAttributeArgCount = 5008,
    SemUnknownAttribute = 5009,

    // ─── Backend (7000-7999) ────────────────────────────────────────────
    BackendUnresolvedSymbol = 7001,
    BackendLinkerError = 7002,
    BackendCodegenError = 7003,
    BackendTargetUnsupported = 7004,

    // ─── Warnings (8000-8999) ───────────────────────────────────────────
    WarnUnreachableCode = 8001,
    WarnUnusedVariable = 8002,
    WarnUnusedParameter = 8003,
    WarnUnusedFunction = 8004,
    WarnDeprecated = 8005,
    WarnUnawaitedAsync = 8006,
    WarnUnjoinedSpawn = 8007,
    WarnUnreachableCase = 8008,
};

// ─── Helpers ─────────────────────────────────────────────────────────────

/// Get the category name from an error code.
inline const char* categoryName(ErrorCode code) {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw >= 1000 && raw < 2000) return "Lexical";
    if (raw >= 2000 && raw < 3000) return "Syntax";
    if (raw >= 3000 && raw < 7000) return "Semantic";
    if (raw >= 7000 && raw < 8000) return "Backend";
    if (raw >= 8000 && raw < 9000) return "Warning";
    return "Unknown";
}

/// Get the severity from an error code.
inline Severity severityFromCode(ErrorCode code) {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw >= 8000) return Severity::Warning;
    return Severity::Error;
}

/// Check if a code is a warning.
inline bool isWarningCode(ErrorCode code) {
    return static_cast<uint32_t>(code) >= 8000;
}

/// Check if a code is an error.
inline bool isErrorCode(ErrorCode code) {
    return !isWarningCode(code);
}

// ─── Diagnostic ──────────────────────────────────────────────────────────

struct Diagnostic {
    Severity severity;
    ErrorCode code;
    SourceLocation location;
    std::string message;

    std::string category() const {
        return categoryName(code);
    }
};

// ─── Diagnostic Context ──────────────────────────────────────────────────

/// @brief Simple diagnostic context - stores and tracks all diagnostics.
///
/// Usage:
///   diagnostic::Context ctx;
///   ctx.error(ErrorCode::SemUndefinedValue, node, "undefined variable '", name, "'");
///   ctx.warning(ErrorCode::WarnUnusedVariable, node, "unused variable '", name, "'");
///   ctx.note(node, "consider using '_' to ignore");
///
///   if (ctx.canContinue()) { ... }
///   ctx.dump(std::cerr);
class Context {
public:
    // ─── Report Functions ──────────────────────────────────────────────

    /// Report an error with a diagnostic code.
    template<typename... Args>
    void error(ErrorCode code, const BaseAST* node, Args&&... args) {
        add(severityFromCode(code), code,
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a warning with a diagnostic code.
    template<typename... Args>
    void warning(ErrorCode code, const BaseAST* node, Args&&... args) {
        add(Severity::Warning, code,
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a free-text note.
    template<typename... Args>
    void note(const BaseAST* node, Args&&... args) {
        add(Severity::Note, ErrorCode(0),
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a free-text hint.
    template<typename... Args>
    void hint(const BaseAST* node, Args&&... args) {
        add(Severity::Hint, ErrorCode(0),
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    // ─── Convenience overloads with explicit location ─────────────────

    template<typename... Args>
    void errorAt(ErrorCode code, const SourceLocation& loc, Args&&... args) {
        add(severityFromCode(code), code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template<typename... Args>
    void warningAt(ErrorCode code, const SourceLocation& loc, Args&&... args) {
        add(Severity::Warning, code, loc,
            buildMessage(std::forward<Args>(args)...));
    }

    template<typename... Args>
    void noteAt(const SourceLocation& loc, Args&&... args) {
        add(Severity::Note, ErrorCode(0), loc,
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
        if (d.code != ErrorCode(0)) {
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

    void add(Severity sev, ErrorCode code, const SourceLocation& loc, std::string msg) {
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

} // namespace diagnostic