#include "DiagnosticMessages.hpp"

namespace DiagnosticMessages {

std::string_view getMessage(DiagCode code) {
    switch (code) {
        case DiagCode::E0001: return "File not found or inaccessible.";
        case DiagCode::E1001: return "Invalid character.";
        case DiagCode::E1002: return "Unterminated string literal.";
        case DiagCode::E1003: return "Unterminated raw string literal.";
        case DiagCode::E1004: return "Mismatched '#' count in raw string literal.";
        case DiagCode::E1005: return "Invalid escape sequence.";
        case DiagCode::E1006: return "Unterminated block comment.";
        case DiagCode::E2001: return "Expected '%s' but found '%s'.";
        case DiagCode::E2002: return "Unexpected token '%s'.";
        case DiagCode::E2003: return "Expected an identifier.";
        case DiagCode::E2004: return "Expected 'in' in for-loop.";
        case DiagCode::E2005: return "Expected a type annotation.";
        case DiagCode::E2006: return "Invalid context for statement or expression.";
        case DiagCode::E2007: return "Duplicate clause in switch, match, or impl.";
        case DiagCode::E2008: return "Expected an expression.";
        case DiagCode::E2009: return "Malformed literal.";
        case DiagCode::E2010: return "Unknown qualifier '%s'.";
        case DiagCode::E2014: return "Visibility modifier not allowed in local declaration.";
        case DiagCode::E2015: return "'~' qualifier on anonymous function (not allowed).";
        case DiagCode::E2016: return "'?' used directly on inline function type; use a type alias.";
        case DiagCode::E2018: return "Missing '->' arrow in from entry.";
        case DiagCode::E2019: return "Missing '=' before function body.";
        case DiagCode::E2020: return "'!' argument pack annotation only allowed in pipeline step.";
        case DiagCode::E2026: return "Chained comparison not allowed; use 'and' explicitly.";
        case DiagCode::E2027: return "Attributes cannot be used on multi‑variable declarations.";
        case DiagCode::E2028: return "Invalid type in result suffix: expected primitive or identifier, found '%s'.";
        case DiagCode::E2029: return "Nested '!' not allowed in type; use a type alias.";
        case DiagCode::W3001: return "'@extern' function declared with 'let' — should be 'const'.";
        case DiagCode::W3002: return "'@extern' function has an empty body '= {}' that will be ignored.";
        case DiagCode::W3003: return "Operation on nullable type; value may be nil at runtime.";
        case DiagCode::W3004: return "Unreachable code after unconditional bind pattern.";
        case DiagCode::W3005: return "'default' arm unreachable (all cases covered).";
        case DiagCode::W3006: return "Non‑void expression result discarded.";
        case DiagCode::W3007: return "Method call on literal may be confusing; consider using a variable.";
        case DiagCode::W3008: return "'impl' block for primitive type may shadow built‑in method.";
        default: return "Unknown diagnostic code.";
    }
}

} // namespace DiagnosticMessages