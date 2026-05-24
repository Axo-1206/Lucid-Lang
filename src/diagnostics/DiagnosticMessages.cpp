/**
 * @file DiagnosticMessages.cpp
 * @brief Implementation of the static diagnostic message table.
 */

#include "DiagnosticMessages.hpp"

namespace DiagnosticMessages {

std::string_view getMessage(DiagCode code) {
    switch (code) {
        // System
        case DiagCode::E0001: return "File not found or inaccessible.";

        // Lexical
        case DiagCode::E1001: return "Invalid character encountered in source.";
        case DiagCode::E1002: return "String literal was not terminated before EOF.";
        case DiagCode::E1003: return "Unterminated raw string literal.";
        case DiagCode::E1004: return "Mismatched '#' count in raw string literal.";
        case DiagCode::E1005: return "Invalid escape sequence in string or character literal.";
        case DiagCode::E1006: return "Unterminated block comment.";

        // Syntax
        case DiagCode::E2001: return "Expected '%s' but found '%s'.";
        case DiagCode::E2002: return "Unexpected token '%s'.";
        case DiagCode::E2003: return "Expected an identifier.";
        case DiagCode::E2004: return "Expected the 'in' keyword in a for-loop.";
        case DiagCode::E2005: return "Expected a type annotation (e.g., int, bool, or custom).";
        case DiagCode::E2006: return "Invalid context for a statement or expression.";
        case DiagCode::E2007: return "Duplicate clause in switch or match.";
        case DiagCode::E2008: return "Expected an expression but found none.";
        case DiagCode::E2009: return "Literal value is malformed.";
        case DiagCode::E2010: return "Unknown or unsupported '@' attribute name.";
        case DiagCode::E2011: return "Wrong number of arguments for '@' attribute.";
        case DiagCode::E2012: return "Unexpected keyword found in a position where an identifier or type was expected.";
        case DiagCode::E2014: return "Invalid visibility modifier in local declaration.";
        case DiagCode::E2015: return "'~' qualifier used on anonymous function (not allowed).";
        case DiagCode::E2016: return "'?' used directly on inline function type; use a type alias.";
        case DiagCode::E2017: return "Multiple parameter groups after return boundary '->' in function type.";
        case DiagCode::E2018: return "Missing '->' arrow in from entry.";
        case DiagCode::E2019: return "Missing '=' before function body.";
        case DiagCode::E2020: return "'!' argument pack annotation only allowed in pipeline step.";
        case DiagCode::E2021: return "Nullable chain '?.' must be terminated by '\?\?'.";
        case DiagCode::E2022: return "'default' arm must be last in match expression.";
        case DiagCode::E2024: return "Method '%s' not found on type '%s'.";
        case DiagCode::E2025: return "Cannot access field '%s' on non-struct/enum type.";
        case DiagCode::E2026: return "Chained comparison not allowed; use 'and' explicitly: 0 < x and x < 10.";
        case DiagCode::E2027: return "Attributes cannot be used on multi‑variable declarations.";
        case DiagCode::E2028: return "Invalid type in result suffix: expected primitive or identifier, found '%s'.";
        case DiagCode::E2029: return "Nested '!' not allowed in type; use a type alias (e.g., 'type Alias = T!E; Alias!F').";

        // Semantic
        case DiagCode::E3001: return "Identifier '%s' used before it was declared.";
        case DiagCode::E3002: return "Type mismatch: expected '%s', got '%s'.";
        case DiagCode::E3003: return "Mismatch between function parameters and call arguments.";
        case DiagCode::E3004: return "Attempted to assign to an immutable value.";
        case DiagCode::E3005: return "Symbol '%s' already declared in this scope.";
        case DiagCode::E3006: return "Missing 'main' entry point.";
        case DiagCode::E3007: return "Invalid signature for the 'main' function: %s";
        case DiagCode::E3008: return "Implicit type conversion not allowed; suggest explicit casting.";
        case DiagCode::E3009: return "Unknown '#...' intrinsic name.";
        case DiagCode::E3010: return "Wrong argument count or type for '#...' intrinsic.";
        case DiagCode::E3011: return "Cannot use '==' on struct type; implement Equatable<T> and use :equals() instead.";
        case DiagCode::E3012: return "Cannot use '==' on function type; function bodies are incomparable.";
        case DiagCode::E3013: return "Cannot use '==' on array type; use collection library comparison function.";
        case DiagCode::E3014: return "Chained comparison not allowed; use 'and' explicitly.";
        case DiagCode::E3015: return "'@aot' and '@jit' are mutually exclusive on the same declaration.";
        case DiagCode::E3016: return "'@%s' is only valid on the 'main' entry point; remove it from '%s'";
        case DiagCode::E3017: return "Generic signature mismatch: 'impl' must match struct declaration exactly.";
        case DiagCode::E3018: return "'impl' target must be a named type (not a structural type without alias).";
        case DiagCode::E3019: return "Generic arity mismatch in 'impl' declaration (parameter count does not match target).";
        case DiagCode::E3020: return "'impl' on primitive or enum cannot have generic parameters.";
        case DiagCode::E3021: return "'from' target type must be a named type (primitive, struct, enum, alias).";
        case DiagCode::E3022: return "'from' entry return type does not match enclosing 'from' target type.";
        case DiagCode::E3023: return "Method call '%s' – no impl found for receiver type.";
        case DiagCode::E3024: return "Trait conformance missing method(s) in 'impl' block.";
        case DiagCode::E3025: return "Method signature mismatch with trait declaration.";
        case DiagCode::E3026: return "Duplicate method name in merged impl blocks for same type.";
        case DiagCode::E3027: return "'await' used outside '~async' function body.";
        case DiagCode::E3028: return "'~async' function called without 'await'.";
        case DiagCode::E3029: return "'return' used inside '~parallel' body.";
        case DiagCode::E3030: return "Writing to outer scope variable from '~parallel' body.";
        case DiagCode::E3031: return "'break' or 'continue' used outside loop.";
        case DiagCode::E3032: return "Mismatched return value count in multi‑assignment RHS.";
        case DiagCode::E3033: return "Assigning to non-lvalue expression.";
        case DiagCode::E3034: return "'const' variable missing initialiser.";
        case DiagCode::E3035: return "'const' initialiser not a compile‑time constant.";
        case DiagCode::E3036: return "'nil' assigned to non‑nullable type.";
        case DiagCode::E3037: return "'?' suffix on type that is already nullable (double‑nullable not allowed).";
        case DiagCode::E3038: return "'~nullable' function call without nil guard.";
        case DiagCode::E3039: return "Generic function call missing explicit type arguments.";
        case DiagCode::E3040: return "Type argument count mismatch in generic instantiation.";
        case DiagCode::E3041: return "'is' expression used outside conditional (statement context only).";
        case DiagCode::E3042: return "Pattern bind name '%s' conflicts with existing variable in scope.";
        case DiagCode::E3043: return "Unconditional bind pattern appears before more specific patterns in match arm.";
        case DiagCode::E3044: return "Every generic parameter declared on a type alias must appear at least once in the right-hand side. Use '@phantom' if this is intentional.";
        case DiagCode::E3045: return "Operation on unresolved '!' type is not allowed. Use 'resolve' or '\?\?' first.";

        // Backend / Codegen
        case DiagCode::E4001: return "Target machine initialisation failed (unknown target triple).";
        case DiagCode::E4002: return "Failed to open output file for AOT object/assembly emission.";
        case DiagCode::E4003: return "AOT object file emission failed (LLVM pass pipeline error).";
        case DiagCode::E4004: return "Linker invocation failed (non-zero exit code).";
        case DiagCode::E4005: return "JIT symbol lookup failed (symbol not found after compilation).";
        case DiagCode::E4006: return "Module verification failed (malformed LLVM IR).";
        case DiagCode::E4007: return "Generic instantiation error (type arg count mismatch vs declaration).";
        case DiagCode::E4008: return "Unresolved generic type instantiation (missing concrete type).";
        case DiagCode::E4009: return "Unimplemented intrinsic called (compiler feature missing).";

        // Warnings
        case DiagCode::W3001: return "'@extern' function declared with 'let' — should be 'const'.";
        case DiagCode::W3002: return "'@extern' function has an empty body '= {}' that will be ignored.";
        case DiagCode::W3003: return "Performing operation on nullable type; value may be nil at runtime.";
        case DiagCode::W3004: return "Unreachable code after unconditional bind pattern.";
        case DiagCode::W3005: return "'default' arm in switch is unreachable (all cases covered).";
        case DiagCode::W3006: return "Non‑void expression result discarded without explicit handling.";
        case DiagCode::W3007: return "Method call on literal may be confusing; consider using a variable.";
        case DiagCode::W3008: return "'impl' block for primitive type may shadow built‑in method.";

        default: return "Unknown diagnostic code.";
    }
}

} // namespace DiagnosticMessages