/**
 * @file Diagnostic.hpp
 *
 * @responsibility Data structures for compiler diagnostics (Errors, Warnings).
 *
 * @usecase Represents a single issue found during any compiler phase.
 */

#pragma once

#include "Tokens.hpp"
#include "ast/BaseAST.hpp"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticSeverity  — Categorizes the impact of the diagnostic
//
// Defines the severity level of an issue, from informational notes up to fatal 
// errors that will halt compilation immediately.
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagnosticSeverity {
    Note,    ///< Informational message or a hint.
    Warning, ///< Potential issue that doesn't stop the build.
    Error,   ///< Requirement violation that prevents a successful build.
    Fatal    ///< Critical failure that causes the compiler to stop immediately.
};

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticCategory  — Identifies which compiler sub-system emitted the diagnostic
//
// Maps the error to the logical phase of the compiler (e.g. Lexer, Parser, 
// Semantic, or Backend errors).
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagnosticCategory {
    General,  ///< Misc or driver errors.
    Lexical,  ///< Scanner/tokenization errors.
    Syntax,   ///< Parser/grammar errors.
    Semantic, ///< Type-checking or logic errors.
    Backend   ///< LLVM IR generation or optimization errors.
};

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostic  — The data packet for a single reporter issue
//
// Carries all necessary information to present a compiler error or warning to 
// the user, including severity, phase, location, error code, and message.
// ─────────────────────────────────────────────────────────────────────────────
struct Diagnostic {
    DiagnosticSeverity severity; ///< Impact level (Error, Warning, etc.).
    DiagnosticCategory category; ///< Compiler phase (Lexical, Syntax, etc.).
    SourceLocation location;     ///< Where in the source the issue was found.
    uint32_t code;               ///< Unique numeric error code (see DiagnosticCodes.hpp).
    std::string message;         ///< Human-readable description of the issue.
};
