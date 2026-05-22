/**
 * @file DiagnosticCodes.hpp
 * @brief Unique numeric error codes grouped by compilation phase.
 *
 * Codes are sequential within each group (starting at a base). The numeric
 * value is used as an index into a static message table.
 *
 * @see DiagnosticMessages.cpp
 */

#pragma once
#include <cstdint>

enum class DiagCode : uint32_t {
    // ========== 0000–0999: System / Driver ==========
    E0001 = 0,   ///< File not found or inaccessible.

    // ========== 1000–1999: Lexical ==========
    E1001 = 1000,  ///< Invalid character encountered.
    E1002,         ///< Unterminated string literal.
    E1003,         ///< Unterminated raw string literal.
    E1004,         ///< Mismatched '#' count in raw string literal.
    E1005,         ///< Invalid escape sequence.
    E1006,         ///< Unterminated block comment.

    // ========== 2000–2999: Syntax ==========
    E2001 = 2000,  ///< Expected token X but found Y.
    E2002,         ///< Token not allowed in this context.
    E2003,         ///< Expected an identifier.
    E2004,         ///< Expected 'in' in for‑loop.
    E2005,         ///< Expected a type annotation.
    E2006,         ///< Invalid statement/expression context.
    E2007,         ///< Duplicate switch/match clause.
    E2008,         ///< Expected an expression.
    E2009,         ///< Malformed literal.
    E2010,         ///< Unknown '@' attribute.
    E2011,         ///< Wrong argument count for '@' attribute.
    E2012,         ///< Unexpected keyword.
    E2014,         ///< Invalid visibility modifier in local declaration.
    E2015,         ///< '~' qualifier on anonymous function.
    E2016,         ///< '?' on inline function type – use alias.
    E2017,         ///< Multiple parameter groups after '->'.
    E2018,         ///< Missing '->' in from entry.
    E2019,         ///< Missing '=' before function body.
    E2020,         ///< '!' only allowed in pipeline step.
    E2021,         ///< Nullable chain '?.' missing '??'.
    E2022,         ///< 'default' arm not last in match.
    E2024,         ///< Method not found on receiver.
    E2025,         ///< Field access on non‑struct/enum.
    E2026,         ///< Chained comparison without 'and'.
    E2027,         ///< Attributes not allowed on multi-variable declaration.

    // ========== 3000–3999: Semantic ==========
    E3001 = 3000,  ///< Undeclared identifier.
    E3002,         ///< Type mismatch.
    E3003,         ///< Parameter/argument count mismatch.
    E3004,         ///< Assignment to immutable value.
    E3005,         ///< Symbol already declared.
    E3006,         ///< Missing 'main' entry point.
    E3007,         ///< Invalid 'main' signature.
    E3008,         ///< Implicit conversion disallowed.
    E3009,         ///< Unknown '#...' intrinsic.
    E3010,         ///< Wrong argument count/type for intrinsic.
    E3011,         ///< '==' on struct – use :equals().
    E3012,         ///< '==' on function type.
    E3013,         ///< '==' on array type.
    E3014,         ///< Chained comparison.
    E3015,         ///< '@aot' and '@jit' together.
    E3016,         ///< '@aot'/'@jit' not on 'main'.
    E3017,         ///< Generic signature mismatch in impl.
    E3018,         ///< Impl target must be named type.
    E3019,         ///< Generic arity mismatch in impl.
    E3020,         ///< Impl on primitive/enum cannot have generics.
    E3021,         ///< From target must be named type.
    E3022,         ///< From entry return type mismatch.
    E3023,         ///< Method not found for receiver.
    E3024,         ///< Trait conformance missing method(s).
    E3025,         ///< Method signature mismatch with trait.
    E3026,         ///< Duplicate method name in impls.
    E3027,         ///< 'await' outside ~async.
    E3028,         ///< ~async call without await.
    E3029,         ///< 'return' in ~parallel body.
    E3030,         ///< Write to outer variable in ~parallel.
    E3031,         ///< break/continue outside loop.
    E3032,         ///< Multi‑assign RHS value count mismatch.
    E3033,         ///< Assigning to non‑lvalue.
    E3034,         ///< 'const' missing initialiser.
    E3035,         ///< 'const' initialiser not compile‑time constant.
    E3036,         ///< 'nil' assigned to non‑nullable type.
    E3037,         ///< Double nullable (e.g., int??).
    E3038,         ///< ~nullable call without nil guard.
    E3039,         ///< Generic call missing type arguments.
    E3040,         ///< Type argument count mismatch.
    E3041,         ///< 'is' expression outside conditional.
    E3042,         ///< Pattern bind name conflicts.
    E3043,         ///< Unconditional bind after specific patterns.

    // ========== 4000–4999: Backend / Codegen ==========
    E4001 = 4000,  ///< Target machine init failed.
    E4002,         ///< Cannot open output file.
    E4003,         ///< AOT emission failed.
    E4004,         ///< Linker invocation failed.
    E4005,         ///< JIT symbol lookup failed.
    E4006,         ///< Module verification failed.
    E4007,         ///< Generic instantiation error.
    E4008,         ///< Unresolved generic type.
    E4009,         ///< Unimplemented intrinsic.

    // ========== 5000–5999: Warnings ==========
    // Warning codes occupy the 5000+ range; they are displayed as W3xxx.
    W3001 = 5000,  ///< @extern with 'let' (should be 'const').
    W3002,         ///< @extern has empty body.
    W3003,         ///< Operation on nullable may be nil.
    W3004,         ///< Unreachable code after unconditional bind.
    W3005,         ///< 'default' arm unreachable.
    W3006,         ///< Non‑void result discarded.
    W3007,         ///< Method call on literal.
    W3008,         ///< Primitive impl shadows built‑in.
};

// Ensure no overflow (last code is below 65535)
static_assert(static_cast<uint32_t>(DiagCode::W3008) < 6000,
              "DiagCode out of expected range");