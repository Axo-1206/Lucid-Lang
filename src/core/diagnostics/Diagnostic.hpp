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

// ─── Diagnostic Codes ──────────────────────────────────────────────────────

/// @brief Unique diagnostic codes with clear category prefixes.
/// 
/// Naming convention: Category_Detail
///   - Lex_*     : Lexical errors (1000-1999)
///   - Syntax_*  : Syntax errors (2000-2999)
///   - Sem_*     : Semantic errors (3000-6999)
///   - Backend_* : Backend errors (7000-7999)
///   - Warn_*    : Warnings (8000-8999)
enum class DiagCode : uint32_t {
    // ─── Lexical (1000-1999) ────────────────────────────────────────────
    Lex_InvalidCharacter         = 1001,
    Lex_UnterminatedString       = 1002,
    Lex_UnterminatedRawString    = 1003,
    Lex_UnterminatedBlockComment = 1004,
    Lex_UnknownCharacter         = 1005,

    // ─── Syntax (2000-2999) ─────────────────────────────────────────────
    Syntax_ExpectedIdentifier       = 2001,
    Syntax_ExpectedType             = 2002,
    Syntax_ExpectedToken            = 2003,
    Syntax_UnexpectedToken          = 2004,
    Syntax_ExpectedExpression       = 2005,
    Syntax_ExpectedBlock            = 2006,
    Syntax_MultipleDefaults         = 2007,
    Syntax_EmptyGroup               = 2008,
    Syntax_ExpectedPipelineSeed     = 2009,
    Syntax_ExpectedModulePath       = 2010,
    Syntax_ExpectedAttributeLiteral = 2011,
    Syntax_ExpectedSwitchSubject    = 2012,

    // ─── Semantic - Name Resolution (3000-3999) ────────────────────────
    Sem_UndefinedValue            = 3001,
    Sem_UndefinedType             = 3002,
    Sem_NotCallable               = 3003,
    Sem_Redeclaration             = 3004,
    Sem_UndefinedModule           = 3005,
    Sem_UndefinedMember           = 3006,
    Sem_GenericParamUnused        = 3007,
    Sem_TraitNotFound             = 3008,
    Sem_NotATrait                 = 3009,
    Sem_FieldNotFound             = 3010,
    Sem_GenericParamRedeclaration = 3011,
    Sem_ImportAliasRedeclaration  = 3012,
    Sem_ModuleNotAnalyzed         = 3013,

    // ─── Semantic - Type Checking (4000-4999) ──────────────────────────
    Sem_TypeMismatch             = 4001,
    Sem_ArgCountMismatch         = 4002,
    Sem_MissingInitializer       = 4003,
    Sem_ConstNullable            = 4004,
    Sem_MissingReturn            = 4005,
    Sem_DuplicateValue           = 4006,
    Sem_UnknownIntrinsic         = 4007,
    Sem_SelfReference            = 4008,
    Sem_InvalidSwitchType        = 4009,
    Sem_MissingCase              = 4010,
    Sem_PipelineMismatch         = 4011,
    Sem_CompositionMismatch      = 4012,
    Sem_RefInStruct              = 4013,
    Sem_IllegalNilErr            = 4014,
    Sem_InvalidGenericArg        = 4015,
    Sem_UnknownType              = 4016,
    Sem_InvalidArrayElement      = 4017,
    Sem_RefInArray               = 4018,
    Sem_FunctionNullable         = 4019,
    Sem_ArrayNullable            = 4020,
    Sem_RefToTrait               = 4021,
    Sem_InvalidPointerTarget     = 4022,
    Sem_InvalidParamType         = 4023,
    Sem_InvalidReturnType        = 4024,
    Sem_ReturnRef                = 4025,
    Sem_ReturnTrait              = 4026,
    Sem_GenericParamNotCallable  = 4027,
    Sem_SelfReferentialInit      = 4028,
    Sem_InvalidAssignment        = 4029,
    Sem_InvalidUnary             = 4030,
    Sem_InvalidBinary            = 4031,

    // ─── Semantic - Generics/Traits/FFI (5000-5999) ────────────────────
    Sem_GenericArityMismatch  = 5001,
    Sem_GenericConstraint     = 5002,
    Sem_TraitImplementation   = 5003,
    Sem_TraitConflict         = 5004,
    Sem_ForeignInvalid        = 5005,
    Sem_ForeignABI            = 5006,
    Sem_AttributeInvalid      = 5007,
    Sem_AttributeArgCount     = 5008,
    Sem_UnknownAttribute      = 5009,
    Sem_GenericParamRequired  = 5010,

    // ─── Backend (7000-7999) ────────────────────────────────────────────
    Backend_UnresolvedSymbol  = 7001,
    Backend_LinkerError       = 7002,
    Backend_CodegenError      = 7003,
    Backend_TargetUnsupported = 7004,

    // ─── Warnings (8000-8999) ───────────────────────────────────────────
    Warn_UnreachableCode   = 8001,
    Warn_UnusedVariable    = 8002,
    Warn_UnusedParameter   = 8003,
    Warn_UnusedFunction    = 8004,
    Warn_Deprecated        = 8005,
    Warn_UnawaitedAsync    = 8006,
    Warn_UnjoinedSpawn     = 8007,
    Warn_UnreachableCase   = 8008,
    Warn_DiscardedResult   = 8009,
    Warn_RedundantNilCheck = 8010,
};


// ─── Helpers ─────────────────────────────────────────────────────────────

/// Get the category name from an error code.
inline const char* categoryName(DiagCode code) {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw >= 1000 && raw < 2000) return "Lexical";
    if (raw >= 2000 && raw < 3000) return "Syntax";
    if (raw >= 3000 && raw < 7000) return "Semantic";
    if (raw >= 7000 && raw < 8000) return "Backend";
    if (raw >= 8000 && raw < 9000) return "Warning";
    return "Unknown";
}

/// Get the severity from an error code.
inline Severity severityFromCode(DiagCode code) {
    uint32_t raw = static_cast<uint32_t>(code);
    if (raw >= 8000) return Severity::Warning;
    return Severity::Error;
}

/// Check if a code is a warning.
inline bool isWarningCode(DiagCode code) {
    return static_cast<uint32_t>(code) >= 8000;
}

/// Check if a code is an error.
inline bool isErrorCode(DiagCode code) {
    return !isWarningCode(code);
}

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
    void error(DiagCode code, const BaseAST* node, Args&&... args) {
        add(severityFromCode(code), code,
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a warning with a diagnostic code.
    template<typename... Args>
    void warning(DiagCode code, const BaseAST* node, Args&&... args) {
        add(Severity::Warning, code,
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a free-text note.
    template<typename... Args>
    void note(const BaseAST* node, Args&&... args) {
        add(Severity::Note, DiagCode(0),
            node ? node->loc : SourceLocation{},
            buildMessage(std::forward<Args>(args)...));
    }

    /// Report a free-text hint.
    template<typename... Args>
    void hint(const BaseAST* node, Args&&... args) {
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
