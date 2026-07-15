#include "DiagnosticMessages.hpp"
#include "DiagnosticCodes.hpp"

namespace diagnosticMessages {

std::string_view getMessage(DiagCode code) {
    switch (code) {
        // ========== 0000–0099: Environment ==========
        case DiagCode::E0001: return "Unknown error.";
        case DiagCode::E0002: return "Too many consecutive errors (%s). Aborting...";
        case DiagCode::E0003: return "File not found or inaccessible.";
        case DiagCode::E0004: return "Module resolution failed. Path: %s";
        case DiagCode::E0005: return "Cyclic module dependency.";

        // ========== 0100–0199: Lexical ==========
        case DiagCode::E0101: return "Invalid character in source (only ASCII allowed).";
        case DiagCode::E0102: return "Unterminated string literal.";
        case DiagCode::E0103: return "Unterminated raw string literal.";
        case DiagCode::E0104: return "Unterminated block comment.";
        case DiagCode::E0105: return "Unknown character.";

        // ========== 1000–1999: Parsing (Syntax) ==========
        case DiagCode::E1001: return "Expected keyword '%s', but found '%s'";
        case DiagCode::E1002: return "Expected an identifier (%s), but found '%s'";
        case DiagCode::E1003: return "Expected type (%s), but found '%s'";
        case DiagCode::E1004: return "Expected '%s' to open %s, but found '%s'";
        case DiagCode::E1005: return "Expected '%s' to close %s, but found '%s'";
        case DiagCode::E1006: return "Expected expression (%s), but found '%s'";
        case DiagCode::E1007: return "Expected token %s, but found '%s'";
        case DiagCode::E1008: return "Unexpected token '%s', expected: %s";
        case DiagCode::E1009: return "Unexpected trailing '%s' in %s";
        case DiagCode::E1010: return "Invalid Context for %s. %s";
        case DiagCode::E1011: return "Expected body (block) for %s, but found %s";
        

        case DiagCode::E1101: return "Expected module path after keyword 'use', but found %s"; 
        // case DiagCode::E1102: return "Expected branch (code block) after condition, but found %s";
        // case DiagCode::E1103: return "Expected 'else' branch (code block), but found %s";
        case DiagCode::E1104: return "Expected argument literal (string, integer, float, bool, or identifier), but found %s";
        case DiagCode::E1105: return "Expected switch subject, but found %s";
        case DiagCode::E1106: return "Empty expression group '()'";
        case DiagCode::E1107: return "Expected pipeline seed, but found %s";
        case DiagCode::E1108: return "Multiple default clauses in switch";
        // case DiagCode::E1109: return "Expected default clause body (block), but found %s";
        
        
        

        default: return "Unknown diagnostic code.";
    }
}

} // namespace diagnosticMessages