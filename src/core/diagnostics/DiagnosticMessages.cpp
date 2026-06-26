#include "DiagnosticMessages.hpp"
#include "DiagnosticCodes.hpp"

namespace diagnosticMessages {

std::string_view getMessage(DiagCode code) {
    switch (code) {
        // ========== 0000–0099: Environment ==========
        case DiagCode::E0001: return "File not found or inaccessible.";
        case DiagCode::E0002: return "Module resolution failed.";
        case DiagCode::E0003: return "Cyclic module dependency.";
        case DiagCode::E0004: return "Invalid package declaration.";

        // ========== 0100–0199: Lexical ==========
        case DiagCode::E0101: return "Invalid character in source (only ASCII allowed).";
        case DiagCode::E0102: return "Unterminated string literal.";
        case DiagCode::E0103: return "Unterminated raw string literal.";
        case DiagCode::E0104: return "Mismatched '#' count in raw string literal.";
        case DiagCode::E0105: return "Invalid escape sequence.";
        case DiagCode::E0106: return "Unterminated block comment.";
        case DiagCode::E0107: return "Unexpected end of file while scanning token.";

        // ========== 1000–1999: Parsing (Syntax) ==========
        case DiagCode::E1001: return "Expected keyword '%s', but found '%s'";
        case DiagCode::E1002: return "Expected an identifier (%s), but found '%s'";
        case DiagCode::E1003: return "Expected type annotation, but found '%s'";
        case DiagCode::E1004: return "Expected '%s' to open %s, but found '%s'";
        case DiagCode::E1005: return "Expected '%s' to close %s, but found '%s'";
        case DiagCode::E1006: return "Expected expression after '=', but found '%s'";
        case DiagCode::E1007: return "Expected '%s' (%s), but found '%s'";
        case DiagCode::E1008: return "Expected type %s, but found '%s'";
        case DiagCode::E1009: return "Invalid Context for %s. %s";

        // case DiagCode::E1101: return "Expected package name, found %s";
        case DiagCode::E1102: return "Expected module path after keyword 'use', but found %s"; 
        // case DiagCode::E1103: return "Expected name alias after keyword 'as', but found %s";
        case DiagCode::E1104: return "Visibility modifier '%s' not allowed in local declaration";
        // case DiagCode::E1105: return "Invalid context: 'use' declaration is not allowed inside a block";
        case DiagCode::E1106: return "Expected string, integer, boolean, or identifier in attribute argument, but found %s";
        case DiagCode::E1107: return "Unexpected tralling comma in %s";
        case DiagCode::E1108: return "Invalid integer literal '%s' for enum variant";
        case DiagCode::E1109: return "Type %s cannot be generic argument";


        // case DiagCode::E1001: return "Expected token '%s' but found '%s'.";
        // case DiagCode::E1002: return "Unexpected token '%s'.";
        // case DiagCode::E1004: return "Expected 'in' in for-loop.";
        // case DiagCode::E1005: return "Expected a type annotation.";
        // case DiagCode::E1006: return "Invalid context for statement or expression: %s";
        // case DiagCode::E1007: return "Malformed literal.";
        // case DiagCode::E1008: return "Expected expression.";
        // case DiagCode::E1009: return "Expected '->' in from entry.";
        // case DiagCode::E1010: return "Expected '=' before function body.";
        // case DiagCode::E1011: return "Missing ')' in parameter group.";
        // case DiagCode::E1012: return "Missing ']' in array type.";
        // case DiagCode::E1013: return "Missing '>' in generic arguments.";
        // case DiagCode::E1014: return "Misplaced visibility modifier (pub/export inside block).";
        // case DiagCode::E1015: return "Qualifier on anonymous function.";
        // case DiagCode::E1016: return "Unknown qualifier '~%s'.";
        // case DiagCode::E1017: return "Qualifier on non‑function type.";
        // case DiagCode::E1018: return "Duplicate qualifier.";
        // case DiagCode::E1019: return "'?' not allowed on inline function type (use alias or ~nullable).";
        // case DiagCode::E1020: return "'!' not allowed on inline function type (use alias).";
        // case DiagCode::E1021: return "Nested '!' in type (use alias).";
        // case DiagCode::E1022: return "Chained comparison (use 'and').";
        // case DiagCode::E1023: return "Mismatched parentheses in curry type.";
        // case DiagCode::E1024: return "Generic array type (e.g., `[_, <T>]`) only allowed as `impl` target, `from` target, or in type alias right‑hand side.";
        // case DiagCode::E1025: return "Missing type annotation for parameter '%s' in function type. Write `(%s Type)` instead.";
        // case DiagCode::E1026: return "Reference type '&T' cannot be used as a generic argument";
        // case DiagCode::E1027: return "Pointer type '*T' cannot be used as a generic argument";
        // case DiagCode::E1028: return "Missing package declaration at file: '%s'";
        
        // ========== 2000–2999: Semantic ==========
        case DiagCode::E2001: return "Identifier '%s' used before it was declared.";
        case DiagCode::E2002: return "Type mismatch: expected '%s', got '%s'.";
        case DiagCode::E2003: return "Parameter/argument count mismatch.";
        case DiagCode::E2004: return "Attempted to assign to an immutable value.";
        case DiagCode::E2005: return "Symbol '%s' already declared in this scope.";
        case DiagCode::E2006: return "Missing 'main' entry point.";
        case DiagCode::E2007: return "Invalid signature for 'main': %s.";
        case DiagCode::E2008: return "No implicit conversion found from '%s' to '%s'; use explicit cast or declare a 'from' conversion.";
        case DiagCode::E2009: return "Cannot use '==' on struct type; implement Equatable<T> and use :equals() instead.";
        case DiagCode::E2010: return "Cannot use '==' on function type; function bodies are incomparable.";
        case DiagCode::E2011: return "Cannot use '==' on array type; use collection library comparison function.";
        case DiagCode::E2012: return "Chained comparison not allowed; use 'and' explicitly.";
        case DiagCode::E2013: return "'@aot' and '@jit' are mutually exclusive on the same declaration.";
        case DiagCode::E2014: return "'@%s' is only valid on the 'main' entry point; remove it from '%s'.";
        case DiagCode::E2015: return "Generic signature mismatch in 'impl' (must match target generic type).";
        case DiagCode::E2016: return "'impl' target must be a named type (primitive, struct, enum, alias).";
        case DiagCode::E2017: return "Generic arguments mismatch in 'impl'.";
        case DiagCode::E2018: return "'impl' on primitive or enum cannot have generic parameters.";
        case DiagCode::E2019: return "'from' entry return type does not match enclosing 'from' target type.";
        case DiagCode::E2020: return "Method call '%s' – no impl found for receiver type.";
        case DiagCode::E2021: return "Trait conformance missing method(s) in 'impl' block.";
        case DiagCode::E2022: return "Method signature mismatch with trait declaration.";
        case DiagCode::E2023: return "Duplicate method name in merged impl blocks for same type.";
        case DiagCode::E2024: return "'~async' function called without 'await'.";
        case DiagCode::E2025: return "'return' used inside '~parallel' body.";
        case DiagCode::E2026: return "Writing to outer scope variable from '~parallel' body.";
        case DiagCode::E2027: return "'break' or 'continue' used outside loop.";
        case DiagCode::E2028: return "Mismatched return value count in multi‑assignment RHS.";
        case DiagCode::E2029: return "Assignment target is not an lvalue (cannot assign to this expression).";
        case DiagCode::E2030: return "'const' variable missing initialiser.";
        case DiagCode::E2031: return "'const' initialiser not a compile‑time constant.";
        case DiagCode::E2032: return "'nil' assigned to non‑nullable type.";
        case DiagCode::E2033: return "'?' suffix on type that is already nullable (double‑nullable not allowed).";
        case DiagCode::E2034: return "Generic function call missing explicit type arguments.";
        case DiagCode::E2035: return "Type argument count mismatch in generic instantiation.";
        case DiagCode::E2036: return "'is' expression used outside conditional (statement context only).";
        case DiagCode::E2037: return "Pattern bind name '%s' conflicts with existing variable in scope.";
        case DiagCode::E2038: return "Unconditional bind pattern appears before more specific patterns in match arm.";
        case DiagCode::E2039: return "Every generic parameter declared on a type alias/struct/function must appear at least once in the right‑hand side. Use '@phantom' if this is intentional.";
        case DiagCode::E2040: return "Operation on unresolved '!' type is not allowed. Use 'resolve' or '\?\?' first.";
        case DiagCode::E2041: return "Duplicate clause in switch, match, or impl.";
        case DiagCode::E2042: return "Pattern bind name '%s' shadows but is not allowed (shadowing disabled).";
        case DiagCode::E2043: return "'return' used inside '~parallel' body.";
        case DiagCode::E2044: return "Function call argument mismatch (expected %d, received %d).";
        case DiagCode::E2045: return "Recursion not allowed in this context.";

        // ========== 3000–3999: Attribute Validation ==========
        case DiagCode::E3001: return "Unknown or unsupported attribute '@%s'.";
        case DiagCode::E3002: return "Wrong number of arguments for attribute '@%s'.";
        case DiagCode::E3003: return "Attribute argument type mismatch (expected string, integer, or identifier).";
        case DiagCode::E3004: return "Attribute '@%s' not allowed on this declaration.";
        case DiagCode::E3005: return "'@extern' missing symbol name.";
        case DiagCode::E3006: return "'@extern' function must be 'const' and have an empty body.";
        case DiagCode::E3007: return "'@link' can only appear once per file (use comma‑separated arguments).";
        case DiagCode::E3008: return "'@packed' only allowed on struct declarations.";
        case DiagCode::E3009: return "'@inline' and '@noinline' are mutually exclusive on the same declaration.";
        case DiagCode::E3010: return "'@deprecated' missing message string.";
        case DiagCode::E3011: return "'@phantom' only allowed on type alias, struct, or function declarations.";
        case DiagCode::E3012: return "'@phantom' used but generic parameter is actually used (remove @phantom).";
        case DiagCode::E3013: return "'@extern' variadic function cannot be used with injection '!'.";

        // ========== 4000–4999: Intrinsic Validation ==========
        case DiagCode::E4001: return "Unknown intrinsic '#%s'.";
        case DiagCode::E4002: return "Wrong argument count for intrinsic '#%s'.";
        case DiagCode::E4003: return "Argument type mismatch for intrinsic '#%s'.";
        case DiagCode::E4004: return "Intrinsic '#%s' only allowed in unsafe context (--unsafe or inside @extern function).";
        case DiagCode::E4005: return "#bitcast size mismatch: source and target sizes differ.";
        case DiagCode::E4006: return "#ptrOffset requires integer offset.";
        case DiagCode::E4007: return "#toRef argument must be a raw pointer (*T).";

        // ========== 5000–5999: Linker / Backend ==========
        case DiagCode::E5001: return "Unresolved external symbol '%s'.";
        case DiagCode::E5002: return "Linker search path does not exist.";
        case DiagCode::E5003: return "Library format not recognised.";
        case DiagCode::E5004: return "Code generation failed (LLVM error).";
        case DiagCode::E5005: return "Target triple not supported.";

        // ========== 6000–6999: Warnings ==========
        case DiagCode::W6001: return "Unreachable code.";
        case DiagCode::W6002: return "Unused variable '%s'.";
        case DiagCode::W6003: return "Unused parameter '%s' (consider `_` or @phantom).";
        case DiagCode::W6004: return "Unused function '%s'.";
        case DiagCode::W6005: return "Deprecated item used: %s.";
        case DiagCode::W6006: return "'@extern' with 'let' (should be 'const').";
        case DiagCode::W6007: return "'@extern' function with non‑empty body (body ignored).";
        case DiagCode::W6008: return "Nullable operation without explicit guard.";
        case DiagCode::W6009: return "Default case in match may never be reached.";
        case DiagCode::W6010: return "Inefficient slice range (exclusive vs inclusive).";
        case DiagCode::W6011: return "'~async' function called but result ignored.";
        case DiagCode::W6012: return "'~nullable' function called without checking result for nil.";
        case DiagCode::W6013: return "Constant folding overflow.";
        case DiagCode::W6014: return "'~nullable' call without nil guard.";
        case DiagCode::W6015: return "'Duplication implementation of trait: %s, for type: %s";

        default: return "Unknown diagnostic code.";
    }
}

} // namespace diagnosticMessages