/// @file ConstEvalHelpers.hpp
/// @brief Small helpers shared by the four ConstEvaluator `.cpp` files.
///
/// These are implementation details of the evaluator. They are not part
/// of `ConstEvaluator.hpp`'s public surface; only the four evaluator
/// `.cpp` files include this header.

#pragma once

#include "core/ast/BaseAST.hpp"      // ConstantValue
#include "core/memory/InternedString.hpp"

#include <cstdint>
#include <string_view>

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Numeric-literal parsing
// ─────────────────────────────────────────────────────────────────────────────
//
// The parser stores a numeric literal's raw lexeme — `"42"`, `"0xFF"`,
// `"0b1010"`, `"0o777"`, `"3.14"`. These helpers read the lexeme and
// return the value, or a failure indicator if the lexeme is malformed.
//
// The parser guarantees a well-formed lexeme: it rejects a malformed
// numeric token at parse time. So the "failure" branch here is
// defensive — it fires only on a compiler bug, not on user error.

/// Parse a decimal, hex, binary, or octal integer lexeme. Returns true
/// and writes `out` on success, false on failure.
bool parseIntLexeme(std::string_view lexeme, int64_t& out);

/// Parse a decimal float lexeme. Returns true and writes `out` on
/// success, false on failure.
bool parseFloatLexeme(std::string_view lexeme, double& out);

// ─────────────────────────────────────────────────────────────────────────────
// Kind predicates
// ─────────────────────────────────────────────────────────────────────────────
//
// Small wrappers over `ConstantValue::kind` that read better at call
// sites inside the folders. They do not duplicate the existing
// `ConstantValue::isXxx()` predicates; they exist because a folder
// often needs "the operand is an integer of any width" as one query
// rather than two.

/// True if the value is a `Bool`.
inline bool isBool(const ConstantValue& v) { return v.isBool(); }

/// True if the value is an `Int` of any width.
inline bool isInt(const ConstantValue& v) { return v.isInt(); }

/// True if the value is a `Float`.
inline bool isFloat(const ConstantValue& v) { return v.isFloat(); }

/// True if the value is numeric — an `Int` or a `Float`.
inline bool isNumeric(const ConstantValue& v) {
    return v.isInt() || v.isFloat();
}

/// True if the value is a `String` or a `Char`.
inline bool isStringLike(const ConstantValue& v) {
    return v.isString() || v.isChar();
}

} // namespace lucid::sema