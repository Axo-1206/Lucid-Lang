#pragma once
#include <cstdint>

enum class DiagCode : uint32_t {
    // ========== 0000–0099: Environment ==========
    E0001 = 0,      ///< File not found or inaccessible.
    E0002,          ///< Module resolution failed.
    E0003,          ///< Cyclic module dependency.
    E0004,          ///< Invalid package declaration.

    // ========== 0100–0199: Lexical ==========
    E0101 = 100,    ///< Invalid character in source (only ASCII allowed).
    E0102,          ///< Unterminated string literal.
    E0103,          ///< Unterminated raw string literal.
    E0104,          ///< Mismatched '#' count in raw string literal.
    E0105,          ///< Invalid escape sequence.
    E0106,          ///< Unterminated block comment.
    E0107,          ///< Unexpected end of file while scanning token.

    // ========== 1000–1999: Parsing (Syntax) ==========
    // General codes
    E1001 = 1000,   ///< Expected keyword
    E1002,          ///< Expected an identifier.
    E1003,          ///< Expected type annotation
    E1004,          ///< Expected '{, [, <, ('
    E1005,          ///< Expected '}, ], >, )'
    E1006,          ///< Expected expression after '='
    E1007,          ///< Expected token
    E1008,          ///< Expected type

    // Speicalize codes
    // E1101 = 1100,   ///< Expected package name
    E1102,          ///< Expected module path after keyword 'use'
    E1103,          ///< Expected name alias after keyword 'as'
    E1104,          ///< Visibility modifier not allowed in local declaration
    E1105,          ///< Invalid context: 'use' declaration is not allowed inside a block
    E1106,          ///< Invalid argument for attribute
    E1107,          ///< Unexpected tralling comma
    E1108,          ///< Invalid enum variant integer literal
    E1109,          ///< Type can't be used as generic argument


    // E1001 = 1000,   ///< Expected token '%s' but found '%s'.
    // E1002,          ///< Unexpected token '%s'.
    
    // E1004,          ///< Expected 'in' in for-loop.
    // E1005,          ///< Expected a type annotation.
    // E1006,          ///< Invalid context for statement or expression.
    // E1007,          ///< Malformed literal.
    // E1008,          ///< Expected expression.
    // E1009,          ///< Expected '->' in from entry.
    // E1010,          ///< Expected '=' before function body.
    // E1011,          ///< Missing ')' in parameter group.
    // E1012,          ///< Missing ']' in array type.
    // E1013,          ///< Missing '>' in generic arguments.
    // E1014,          ///< Misplaced visibility modifier (pub/export inside block).
    // E1015,          ///< Qualifier on anonymous function.
    // E1016,          ///< Unknown qualifier '~%s'.
    // E1017,          ///< Qualifier on non‑function type.
    // E1018,          ///< Duplicate qualifier.
    // E1019,          ///< '?' not allowed on inline function type (use alias or ~nullable).
    // E1020,          ///< '!' not allowed on inline function type (use alias).
    // E1021,          ///< Nested '!' in type (use alias).
    // E1022,          ///< Chained comparison (use 'and').
    // E1023,          ///< Mismatched parentheses in curry type.
    // E1024,          ///< Invalid context for generic array
    // E1025,          ///< Missing type annotation in function type parameter (e.g., `(T) -> U` instead of `(t T) -> U`)
    // E1026,          ///< Generic argument cannot be a reference type (&T).
    // E1027,          ///< Generic argument cannot be a pointer type (*T).
    // E1028,          ///< Missing package declaration

    // ========== 2000–2999: Semantic ==========
    E2001 = 2000,   ///< Undeclared identifier '%s'.
    E2002,          ///< Type mismatch: expected '%s', got '%s'.
    E2003,          ///< Parameter/argument count mismatch.
    E2004,          ///< Assignment to immutable value.
    E2005,          ///< Symbol '%s' already declared in this scope.
    E2006,          ///< Missing 'main' entry point.
    E2007,          ///< Invalid signature for 'main': %s.
    E2008,          ///< No implicit conversion found from '%s' to '%s'; use explicit cast or declare a 'from' conversion.
    E2009,          ///< '==' on struct – use :equals() from Equatable.
    E2010,          ///< '==' on function type (not comparable).
    E2011,          ///< '==' on array type (use collection comparison).
    E2012,          ///< Chained comparison (use 'and').
    E2013,          ///< '@aot' and '@jit' together on same declaration.
    E2014,          ///< '@aot' or '@jit' only valid on 'main'.
    E2015,          ///< Generic signature mismatch in impl (must match target generic type).
    E2016,          ///< Impl target must be a named type (primitive, struct, enum, alias).
    E2017,          ///< Generic arguments mismatch in impl.
    E2018,          ///< Impl on primitive/enum cannot have generic parameters.
    E2019,          ///< From entry return type mismatch with enclosing from target.
    E2020,          ///< Method '%s' not found for receiver type.
    E2021,          ///< Trait conformance missing method(s).
    E2022,          ///< Method signature mismatch with trait.
    E2023,          ///< Duplicate method name in impl blocks for same type.
    E2024,          ///< ~async call without 'await'.
    E2025,          ///< 'return' used inside ~parallel body.
    E2026,          ///< Write to outer variable in ~parallel.
    E2027,          ///< 'break' or 'continue' outside loop.
    E2028,          ///< Multi‑assign RHS value count mismatch.
    E2029,          ///< Assignment target is not an lvalue (cannot assign to this expression).
    E2030,          ///< 'const' missing initialiser.
    E2031,          ///< 'const' initialiser not compile‑time constant.
    E2032,          ///< 'nil' assigned to non‑nullable type.
    E2033,          ///< Double nullable (e.g., int??).
    E2034,          ///< Generic call missing type arguments.
    E2035,          ///< Type argument count mismatch.
    E2036,          ///< 'is' expression outside conditional (statement context only).
    E2037,          ///< Pattern bind name '%s' conflicts with existing variable.
    E2038,          ///< Unconditional bind after more specific patterns in match.
    E2039,          ///< Generic parameter unused in type alias/struct/function (add @phantom).
    E2040,          ///< Operation on unresolved '!' type (use resolve or ??).
    E2041,          ///< Duplicate clause in switch, match, or impl.
    E2042,          ///< Pattern bind name shadows but not allowed.
    E2043,          ///< Return in function that cannot return (e.g., ~parallel).
    E2044,          ///< Function call argument mismatch (expected %d, received %d).
    E2045,          ///< Recursion not allowed in this context.

    // ========== 3000–3999: Attribute Validation ==========
    E3001 = 3000,   ///< Unknown attribute '@%s'.
    E3002,          ///< Wrong argument count for attribute '@%s'.
    E3003,          ///< Attribute argument type mismatch (expected string, integer, or identifier).
    E3004,          ///< Attribute '@%s' not allowed on this declaration.
    E3005,          ///< '@extern' missing symbol name.
    E3006,          ///< '@extern' function must be 'const' and have empty body.
    E3007,          ///< '@link' can only appear once per file (use comma‑separated arguments).
    E3008,          ///< '@packed' only allowed on struct.
    E3009,          ///< '@inline' and '@noinline' mutually exclusive.
    E3010,          ///< '@deprecated' missing message string.
    E3011,          ///< '@phantom' only allowed on type alias, struct, or function.
    E3012,          ///< '@phantom' used but generic parameter is actually used.
    E3013,          ///< '@extern' variadic function cannot be used with injection '!'.

    // ========== 4000–4999: Intrinsic Validation ==========
    E4001 = 4000,   ///< Unknown intrinsic '#%s'.
    E4002,          ///< Wrong argument count for intrinsic '#%s'.
    E4003,          ///< Argument type mismatch for intrinsic '#%s'.
    E4004,          ///< Intrinsic '#%s' only allowed in unsafe context (--unsafe or @extern).
    E4005,          ///< #bitcast size mismatch (source and target sizes differ).
    E4006,          ///< #ptrOffset requires integer offset.
    E4007,          ///< #toRef argument must be a raw pointer (*T).

    // ========== 5000–5999: Linker / Backend ==========
    E5001 = 5000,   ///< Unresolved external symbol '%s'.
    E5002,          ///< Linker search path does not exist.
    E5003,          ///< Library format not recognised.
    E5004,          ///< Code generation failed (LLVM error).
    E5005,          ///< Target triple not supported.

    // ========== 6000–6999: Warnings ==========
    W6001 = 6000,   ///< Unreachable code.
    W6002,          ///< Unused variable '%s'.
    W6003,          ///< Unused parameter '%s' (consider `_` or @phantom).
    W6004,          ///< Unused function '%s'.
    W6005,          ///< Deprecated item used: %s.
    W6006,          ///< @extern with 'let' (should be 'const').
    W6007,          ///< @extern function with non‑empty body (body ignored).
    W6008,          ///< Nullable operation without explicit guard.
    W6009,          ///< Default case in match may never be reached.
    W6010,          ///< Inefficient slice range (exclusive vs inclusive).
    W6011,          ///< ~async function called but result ignored.
    W6012,          ///< ~nullable function called without checking result for nil.
    W6013,          ///< Constant folding overflow.
    W6014,          ///< ~nullable call without nil guard.
    W6015,          ///< Duplication implementation for trait
};

static_assert(static_cast<uint32_t>(DiagCode::W6014) < 7000, "range check");