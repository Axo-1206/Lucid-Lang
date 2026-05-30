#pragma once
#include <cstdint>

enum class DiagCode : uint32_t {
    // ========== 0000–0999: System ==========
    E0001 = 0,   ///< File not found or inaccessible.

    // ========== 1000–1999: Lexical ==========
    E1001 = 1000,  ///< Invalid character.
    E1002,         ///< Unterminated string.
    E1003,         ///< Unterminated raw string.
    E1004,         ///< Mismatched '#' in raw string.
    E1005,         ///< Invalid escape.
    E1006,         ///< Unterminated block comment.

    // ========== 2000–2999: Syntax ==========
    E2001 = 2000,  ///< Expected token X but found Y.
    E2002,         ///< Unexpected token.
    E2003,         ///< Expected identifier.
    E2004,         ///< Expected 'in'.
    E2005,         ///< Expected type annotation.
    E2006,         ///< Invalid context.
    E2007,         ///< Duplicate clause.
    E2008,         ///< Expected expression.
    E2009,         ///< Malformed literal.
    // E2010,         ///< Unknown qualifier.
    E2014,         ///< Visibility in local.
    E2015,         ///< Qualifier on anonymous function.
    // E2016,         ///< '?' on inline function type.
    // E2018,         ///< Missing '->' in from entry.
    // E2019,         ///< Missing '=' before body.
    // E2020,         ///< '!' only in pipeline step.
    // E2026,         ///< Chained comparison (use 'and').
    E2027,         ///< Attributes on multi-var decl.
    // E2028,         ///< Invalid type after '!'.
    // E2029,         ///< Nested '!'.

    // ========== 3000–3999: Semantic (reserved for later) ==========
    // None currently used – add as needed.

    // ========== 4000–4999: Backend ==========
    // None yet.

    // ========== 5000–5999: Warnings ==========
    W3001 = 5000,  ///< @extern with 'let'.
    W3002,         ///< @extern empty body.
    W3003,         ///< Nullable operation.
    W3004,         ///< Unreachable code.
    W3005,         ///< Default unreachable.
};

static_assert(static_cast<uint32_t>(DiagCode::W3005) < 6000, "range check");