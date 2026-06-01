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
    E3044,         ///< Generic parameter unused in type alias – add '@phantom'.
    E3045,         ///< Operation on unresolved '!' type.   
    E3046,         ///< Unknown '@' attribute.
    E3047,         ///< Wrong argument count for '@' attribute.

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