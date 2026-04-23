/**
 * @file DiagnosticCodes.hpp
 * 
 * @responsibility Numeric error codes grouped by compilation phase (1000+).
 * 
 * @usecase Bridges the human-readable errors to unique numeric identifiers.
 *
 * @note This file MUST be synchronized with docs/LUC_DIAGNOSTIC_CODES.md.
 *       New codes should be documented in the registry before being added here.
 */

#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// DiagCode  — Unique numeric identifier for every possible compiler issue
//
// The codes are grouped into logical ranges to make identification easier:
//   - 0xxx: System / Environment / Driver
//   - 1xxx: Lexical Analysis (Scanner)
//   - 2xxx: Syntax Analysis (Parser)
//   - 3xxx: Semantic Analysis (Type Checker)
//   - 4xxx: Code Generation (Backend)
// ─────────────────────────────────────────────────────────────────────────────
enum class DiagCode : uint32_t {
    // ── 0000-0999: System / Driver ───────────────────────────────────────────
    E0001 = 0001, ///< File not found or inaccessible.

    // ── 1000-1999: Lexical ───────────────────────────────────────────────────
    E1001 = 1001, ///< Invalid character encountered in source.
    E1002 = 1002, ///< String literal was not terminated before EOF.

    // ── 2000-2999: Syntax ────────────────────────────────────────────────────
    E2001 = 2001, ///< Expected a specific token but found another.
    E2002 = 2002, ///< Token found in a context where it is not allowed.
    E2003 = 2003, ///< Expected an IDENTIFIER (e.g., name of a struct or enum).
    E2004 = 2004, ///< Expected the 'in' keyword in a for-loop.
    E2005 = 2005, ///< Expected a type annotation (e.g., int, bool, or custom).
    E2006 = 2006, ///< Invalid context for a statement or expression.
    E2007 = 2007, ///< Duplicate clause in switch or match.
    E2008 = 2008, ///< Expected an expression but found none.
    E2009 = 2009, ///< Literal value is malformed (e.g., invalid hex sequence).
    E2010 = 2010, ///< Unknown or unsupported '@' attribute name.
    E2011 = 2011, ///< Wrong number of arguments for '@' attribute.
    E2999 = 2999, ///< Generic fallback for syntax errors.

    // ── 3000-3999: Semantic ──────────────────────────────────────────────────
    E3001 = 3001, ///< Identifier used before it was declared.
    E3002 = 3002, ///< Type mismatch between expected and actual expression.
    E3003 = 3003, ///< Mismatch between function parameters and call arguments.
    E3004 = 3004, ///< Attempted to assign to an immutable value.
    E3005 = 3005, ///< Symbol already declared in this scope.
    E3006 = 3006, ///< Missing 'main' entry point.
    E3007 = 3007, ///< Invalid signature for the 'main' function.
    E3008 = 3008, ///< Implicit type conversion not allowed; suggest explicit casting.
    E3009 = 3009, ///< Unknown '@' intrinsic name.
    E3010 = 3010, ///< Wrong argument count or type for '@' intrinsic.

    // ── W3000-W3999: Semantic Warnings ──────────────────────────────────────
    // Warning codes occupy the 5000+ range to avoid collision with error codes.
    // Displayed as W3001, W3002, etc. in diagnostic output.
    W3001 = 5001, ///< '@extern' function declared with 'let' — should be 'const'.
    W3002 = 5002, ///< '@extern' function has an empty body '= {}' that will be ignored.
};