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
    E2012 = 2012, ///< Unexpected keyword found in a position where an identifier or type was expected.
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
    E3011 = 3011, ///< Cannot use '==' on struct type; implement Equatable<T> and use :equals() instead.
    E3012 = 3012, ///< Cannot use '==' on function type; function bodies are incomparable.
    E3013 = 3013, ///< Cannot use '==' on array type; use collection library comparison function.
    E3014 = 3014, ///< Chained comparison not allowed; use 'and' explicitly: 0 < x and x < 10.
    E3015 = 3015, ///< '@aot' and '@jit' are mutually exclusive on the same declaration.
    E3016 = 3016, ///< '@aot' / '@jit' are only valid on the 'main' entry point.
    E3017 = 3017, ///< Generic signature mismatch in 'impl' block.

    // ── 4000-4999: Code Generation ───────────────────────────────────────────
    E4001 = 4001, ///< Target machine initialisation failed (unknown target triple).
    E4002 = 4002, ///< Failed to open output file for AOT object/assembly emission.
    E4003 = 4003, ///< AOT object file emission failed (LLVM pass pipeline error).
    E4004 = 4004, ///< Linker invocation failed (non-zero exit code).
    E4005 = 4005, ///< JIT symbol lookup failed (symbol not found after compilation).
    E4006 = 4006, ///< Module verification failed (malformed LLVM IR).
    E4007 = 4007, ///< Generic instantiation error (type arg count mismatch vs declaration).

    // ── W5000-W5999: Semantic Warnings ──────────────────────────────────────
    // Warning codes occupy the 5000+ range to avoid collision with error codes.
    // Displayed as W3001, W3002, etc. in diagnostic output.
    W3001 = 5001, ///< '@extern' function declared with 'let' — should be 'const'.
    W3002 = 5002, ///< '@extern' function has an empty body '= {}' that will be ignored.
};