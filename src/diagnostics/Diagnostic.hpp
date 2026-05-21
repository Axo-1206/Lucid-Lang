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
#include "ast/support/InternedString.hpp"
#include "DiagnosticCodes.hpp"
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticSeverity  — Categorizes the impact of the diagnostic
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagnosticSeverity {
    Note,    ///< Informational message or a hint.
    Warning, ///< Potential issue that doesn't stop the build.
    Error,   ///< Requirement violation that prevents a successful build.
    Fatal    ///< Critical failure that causes the compiler to stop immediately.
};

// ─────────────────────────────────────────────────────────────────────────────
// DiagnosticCategory  — Identifies which compiler sub-system emitted the diagnostic
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
// the user, including severity, phase, location, file path, error code, and
// format arguments for the message template.
// ─────────────────────────────────────────────────────────────────────────────
struct Diagnostic {
    DiagnosticSeverity severity;          ///< Impact level (Error, Warning, etc.).
    DiagnosticCategory category;          ///< Compiler phase (Lexical, Syntax, etc.).
    InternedString file;                  ///< Source file path (interned).
    SourceLocation location;              ///< Line/column within the file.
    DiagCode code;                        ///< Unique numeric error code.
    std::vector<std::string> args;        ///< Format arguments for %s placeholders.
};