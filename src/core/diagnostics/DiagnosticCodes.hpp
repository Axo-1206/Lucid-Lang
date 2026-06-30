#pragma once
#include <cstdint>

enum class DiagCode : uint32_t {
    // ========== 0000–0099: Environment ==========
    E0001 = 0,      ///< Unknown Error
    E0002,          ///< Too many consecutive errors. Aborting...
    E0003,          ///< File not found or inaccessible.
    E0004,          ///< Module resolution failed.
    E0005,          ///< Cyclic module dependency.

    // ========== 0100–0199: Lexical ==========
    E0101 = 100,    ///< Invalid character in source (only ASCII allowed).
    E0102,          ///< Unterminated string literal.
    E0103,          ///< Unterminated raw string literal.
    E0104,          ///< Unterminated block comment.
    E0105,          ///< Uknown character.

    // ========== 1000–1999: Parsing (Syntax) ==========
    // General codes
    E1001 = 1000,   ///< Expected keyword
    E1002,          ///< Expected an identifier.
    E1003,          ///< Expected type
    E1004,          ///< Expected '{, [, <, ('
    E1005,          ///< Expected '}, ], >, )'
    E1006,          ///< Expected expression after '='
    E1007,          ///< Expected token
    E1008,          ///< Unexpected token
    // E1009,          ///< Expected type
    E1010,          ///< Invalid context

    // Speicalize codes
    E1101,          ///< Expected module path after keyword 'use'
    E1102,          ///< Expected name alias after keyword 'as'
    E1103,          ///< Trailing tokens
    E1104,          ///< Expected attribute argument literal
    E1105,          ///< Expected anon func or ref func

    // E1105,          ///< Unexpected trailing '+' in generic constraints
    // E1106,          ///< Unexpected trailing '.' in path

   



    // ========== 5000–5999: Linker / Backend ==========
    E5001 = 5000,   ///< Unresolved external symbol '%s'.
    E5002,          ///< Linker search path does not exist.
    E5003,          ///< Library format not recognised.
    E5004,          ///< Code generation failed (LLVM error).
    E5005,          ///< Target triple not supported.

    // ========== 6000–6999: Warnings ==========
    W0001 = 6000,   ///< Unknown Warning
    W0002,          ///< Unreachable code.
    // W6002,          ///< Unused variable '%s'.
    // W6003,          ///< Unused parameter '%s' (consider `_` or @phantom).
    // W6004,          ///< Unused function '%s'.
    // W6005,          ///< Deprecated item used: %s.
    // W6006,          ///< @extern with 'let' (should be 'const').
    // W6007,          ///< @extern function with non‑empty body (body ignored).
    // W6008,          ///< Nullable operation without explicit guard.
    // W6009,          ///< Default case in match may never be reached.
    // W6010,          ///< Inefficient slice range (exclusive vs inclusive).
    // W6011,          ///< ~async function called but result ignored.
    // W6012,          ///< ~nullable function called without checking result for nil.
    // W6013,          ///< Constant folding overflow.
    // W6014,          ///< ~nullable call without nil guard.
    // W6015,          ///< Duplication implementation for trait
};

static_assert(static_cast<uint32_t>(DiagCode::W0001) < 7000, "range check");