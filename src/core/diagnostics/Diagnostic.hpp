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
/// @design_decision Error codes represent WHAT the error is, not WHEN it occurs.
///   - A division by zero is a division by zero whether at compile-time or runtime
///   - A type mismatch is a type mismatch whether in const eval or normal code
///   - This eliminates duplicate codes and enables unified error handling
///
/// @design_decision Code Ranges by Semantic Category
///   1000-1999: Lexical Errors
///   2000-2999: Syntax Errors  
///   3000-3999: Name Resolution
///   4000-4999: Type & Value Errors (compile-time + runtime)
///   5000-5499: FFI/Foreign Errors (compile-time + runtime)
///   5500-5999: Control Flow & Concurrency
///   6000-6499: Generics & Traits
///   6500-6999: Memory & Ownership
///   7000-7999: Backend & Linking (truly phase-specific)
///   8000-8999: Warnings (cross-cutting)
enum class DiagCode : uint32_t {
    // ──────────────────────────────────────────────────────────────────────────
    // LEXICAL ERRORS (1000-1999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Lex_InvalidCharacter         = 1001,
    Lex_UnterminatedString       = 1002,
    Lex_UnterminatedRawString    = 1003,
    Lex_UnterminatedBlockComment = 1004,
    Lex_UnknownCharacter         = 1005,
    Lex_InvalidEscapeSequence    = 1006,
    Lex_InvalidNumberLiteral     = 1007,
    Lex_UnterminatedCharLiteral  = 1008,

    // ──────────────────────────────────────────────────────────────────────────
    // SYNTAX ERRORS (2000-2999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Syntax_ExpectedIdentifier               = 2001,
    Syntax_ExpectedType                     = 2002,
    Syntax_ExpectedToken                    = 2003,
    Syntax_UnexpectedToken                  = 2004,
    Syntax_ExpectedExpression               = 2005,
    Syntax_ExpectedBlock                    = 2006,
    Syntax_MultipleDefaults                 = 2007,
    Syntax_EmptyGroup                       = 2008,
    Syntax_ExpectedPipelineSeed             = 2009,
    Syntax_ExpectedModulePath               = 2010,
    Syntax_ExpectedAttributeLiteral         = 2011,
    Syntax_ExpectedSwitchSubject            = 2012,
    Syntax_ExpectedCaseValue                = 2013,
    Syntax_ExpectedForBinding               = 2014,
    Syntax_ExpectedRangeBound               = 2015,
    Syntax_InvalidAttributeTarget           = 2016,
    Syntax_MissingAttributeArgs             = 2017,
    Syntax_TrailingComma                    = 2018,
    Syntax_ExpectedLiteral                  = 2019,
    Syntax_UnexpectedColonAfterField        = 2020,
    Syntax_AnonymousFunctionAtDeclaration   = 2021,

    // ──────────────────────────────────────────────────────────────────────────
    // NAME RESOLUTION ERRORS (3000-3999)
    // ──────────────────────────────────────────────────────────────────────────
    
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
    Sem_AmbiguousName             = 3014,
    Sem_PrivateMember             = 3015,
    Sem_ModuleCycle               = 3016,

    // ──────────────────────────────────────────────────────────────────────────
    // TYPE & VALUE ERRORS (4000-4999)
    // These apply to BOTH compile-time and runtime evaluation.
    // ──────────────────────────────────────────────────────────────────────────
    
    // Type System (4000-4099)
    Sem_TypeMismatch             = 4001,
    Sem_ArgCountMismatch         = 4002,
    Sem_MissingInitializer       = 4003,
    Sem_ConstNullable            = 4004,
    Sem_MissingReturn            = 4005,
    Sem_DuplicateValue           = 4006,
    Sem_UnknownIntrinsic         = 4007,
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
    Sem_InvalidRange             = 4032,
    
    // Arithmetic & Numeric Errors (shared with runtime)
    Sem_DivisionByZero           = 4101,  // Division or modulo by zero
    Sem_IntegerOverflow          = 4102,  // Integer overflow
    Sem_InvalidShift             = 4103,  // Invalid shift operation
    Sem_NegativeShift            = 4104,  // Negative shift amount
    Sem_InvalidCast              = 4105,  // Invalid type cast/conversion
    Sem_CircularDependency       = 4106,  // Circular dependency (const or otherwise)
    Sem_InvalidBitwiseOp         = 4107,  // Bitwise op on non-integer
    Sem_InvalidLogicalOp         = 4108,  // Logical op on non-bool
    Sem_NumericOverflow          = 4109,  // General numeric overflow
    Sem_InvalidIterator          = 4110,  // Invalid for-loop iterator
    
    // Assignment & Mutability (4200-4299)
    Sem_ConstAssignment          = 4200,  // Assigning to const variable
    Sem_ReadOnlyField            = 4201,  // Assigning to const struct field
    Sem_ModuleReadOnly           = 4202,  // Assigning to module member
    Sem_NonLValueAssignment      = 4203,  // Assignment to non-lvalue
    Sem_ConstParamAssignment     = 4204,  // Assigning to const parameter
    Sem_MissingFuncBody          = 4205,  // The function was initialized with missing body
    
    // Fallible/Nullable Errors (4300-4399)
    Sem_UnhandledNil             = 4300,  // Nil not handled
    Sem_UnhandledErr             = 4301,  // Err not handled
    Sem_InvalidNilCheck          = 4302,  // Nil check on non-nullable
    Sem_InvalidErrCheck          = 4303,  // Err check on non-fallible
    Sem_NilInConst               = 4304,  // Nil not allowed in const context
    Sem_ErrInConst               = 4305,  // Err not allowed in const context

    // ──────────────────────────────────────────────────────────────────────────
    // FFI/FOREIGN ERRORS (5000-5499)
    // These can happen at compile-time OR runtime.
    // ──────────────────────────────────────────────────────────────────────────
    
    Ffi_UnknownSymbol        = 5001,  // Symbol not found (link-time error)
    Ffi_SymbolMismatch       = 5002,  // Symbol type/signature mismatch
    Ffi_ABIIncompatible      = 5003,  // ABI/calling convention mismatch
    Ffi_InvalidPointer       = 5004,  // Invalid pointer usage (e.g., &T vs *T)
    Ffi_InvalidForeign       = 5005,  // Invalid @[foreign] declaration
    Ffi_ConstContext         = 5006,  // Foreign call in const context (can't eval)
    Ffi_TypeNotFFI           = 5007,  // Type can't be passed to FFI
    Ffi_UnsafeReturn         = 5008,  // Returning pointer to stack memory
    Ffi_LibraryNotFound      = 5009,  // Library not found at link time
    Ffi_SymbolNotFound       = 5010,  // Symbol not found in library
    Ffi_InvalidABIAttribute  = 5011,  // Invalid ABI attribute value
    Ffi_MissingLibrary       = 5012,  // Missing library specification
    Ffi_UnsupportedType      = 5013,  // Type not supported by FFI
    Ffi_InvalidParamPassing  = 5014,  // Invalid parameter passing mode
    Ffi_VariadicMismatch     = 5015,  // Variadic argument mismatch
    Ffi_ReturnMismatch       = 5016,  // Return type mismatch with C
    Ffi_SizeMismatch         = 5017,  // Size mismatch for struct/union

    // ──────────────────────────────────────────────────────────────────────────
    // CONTROL FLOW & CONCURRENCY ERRORS (5500-5999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Sem_InvalidBreak          = 5501,  // Break outside loop
    Sem_InvalidContinue       = 5502,  // Continue outside loop
    Sem_UnawaitedAsync        = 5503,  // Async never awaited (warning)
    Sem_UnjoinedSpawn         = 5504,  // Spawn never joined (warning)
    Sem_AsyncOutsideFunction  = 5505,  // Async outside function body
    Sem_SpawnOutsideFunction  = 5506,  // Spawn outside function body
    Sem_AwaitOutsideFunction  = 5507,  // Await outside function body
    Sem_JoinOutsideFunction   = 5508,  // Join outside function body
    Sem_AwaitNonAsync         = 5509,  // Await on non-async value
    Sem_JoinNonSpawn          = 5510,  // Join on non-spawn value
    Sem_DoubleAwait           = 5511,  // Awaiting already awaited value
    Sem_DoubleJoin            = 5512,  // Joining already joined value
    Sem_ReturnInAsync         = 5513,  // Return in async context
    Sem_ReturnInSpawn         = 5514,  // Return in spawn context
    Sem_SwitchExhaustive      = 5515,  // Switch not exhaustive
    Sem_DefaultNotLast        = 5516,  // Default clause not last
    Sem_DuplicateCase         = 5517,  // Duplicate case value

    // ──────────────────────────────────────────────────────────────────────────
    // GENERICS & TRAITS ERRORS (6000-6499)
    // ──────────────────────────────────────────────────────────────────────────

    Sem_GenericArityMismatch  = 6001,
    Sem_GenericConstraint     = 6002,
    Sem_TraitImplementation   = 6003,
    Sem_TraitConflict         = 6004,
    Sem_ForeignInvalid        = 6005,
    Sem_ForeignABI            = 6006,
    Sem_AttributeInvalid      = 6007,
    Sem_AttributeArgCount     = 6008,
    Sem_UnknownAttribute      = 6009,
    Sem_GenericParamRequired  = 6010,
    Sem_GenericParamInference = 6011,
    Sem_GenericInstantiate    = 6012,
    Sem_TraitFieldMissing     = 6013,
    Sem_TraitFieldTypeMismatch = 6014,
    Sem_TraitConstMismatch    = 6015,
    Sem_TraitDuplicate        = 6016,
    Sem_GenericCycle          = 6017,
    Sem_AttributeArgValue     = 6018,
    Sem_AttributeDuplicate    = 6019, // Duplicate attribute on same declaration

    // ──────────────────────────────────────────────────────────────────────────
    // MEMORY & OWNERSHIP ERRORS (6500-6999)
    // ──────────────────────────────────────────────────────────────────────────
    
    Sem_InvalidRef            = 6501,  // Invalid reference
    Sem_RefEscape             = 6502,  // Reference escapes scope
    Sem_UseAfterFree          = 6503,  // Use after free
    Sem_DoubleFree            = 6504,  // Double free
    Sem_InvalidPtr            = 6505,  // Invalid pointer operation
    Sem_PtrArithmetic         = 6506,  // Invalid pointer arithmetic
    Sem_PtrDeref              = 6507,  // Invalid pointer dereference
    Sem_RefToStack            = 6508,  // Reference to stack-allocated data
    Sem_MoveAfterUse          = 6509,  // Move after use
    Sem_UninitVariable        = 6510,  // Uninitialized variable
    Sem_InvalidCapture        = 6511,  // Can't capture borrowed type in closure

    // ──────────────────────────────────────────────────────────────────────────
    // BACKEND & LINKING ERRORS (7000-7999)
    // These are truly phase-specific - only happen during codegen/linking.
    // ──────────────────────────────────────────────────────────────────────────
    
    Backend_LinkerError       = 7001,
    Backend_CodegenError      = 7002,
    Backend_TargetUnsupported = 7003,
    Backend_OutOfMemory       = 7004,
    Backend_InvalidIR         = 7005,
    Backend_OptimizerError    = 7006,
    Backend_ObjectWriteError  = 7007,
    Backend_AsmError          = 7008,
    Backend_RelocationError   = 7009,

    // ──────────────────────────────────────────────────────────────────────────
    // WARNINGS (8000-8999)
    // Cross-cutting - all phases can emit these.
    // ──────────────────────────────────────────────────────────────────────────
    
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
    Warn_ShadowedName      = 8011,
    Warn_UnusedImport      = 8012,
    Warn_UnusedType        = 8013,
    Warn_UnusedField       = 8014,
    Warn_IneffectiveConst  = 8015,  // Const doesn't help optimization
    Warn_PotentialOverflow = 8016,  // Potential integer overflow
    Warn_Fallthrough       = 8017,  // Missing case in switch
    Warn_UnsafeFFI         = 8018,  // Unsafe FFI usage
    Warn_ForeignBody       = 8019,  // Foreign function has a body
    Warn_ForeignInline     = 8020,  // Cannot inline foreign function
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
