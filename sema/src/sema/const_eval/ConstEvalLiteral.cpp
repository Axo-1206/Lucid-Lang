/// @file ConstEvalLiteral.cpp
/// @brief Literal folding: the eight literal kinds.
///
/// The parser stores a literal's raw lexeme. This file reads it and
/// produces the typed `ConstantValue`.
///
/// Numeric literals come in four radices for integers (`42`, `0xFF`,
/// `0b1010`, `0o777`) and one shape for floats (`3.14`, `3.14e10`,
/// `3.14e-10`). The lexeme is parsed with `std::from_chars`, which
/// handles the radix directly and rejects trailing junk.
///
/// String and char literals are stored pre-decoded: the lexer has
/// already stripped the surrounding quotes and processed escapes. The
/// evaluator returns the interned lexeme as-is.

#include "ConstEvaluator.hpp"
#include "ConstEvalHelpers.hpp"

#include "sema/context/SemaContext.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <charconv>
#include <cstdlib>

using namespace lucid::diag;

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Lexeme parsing
// ─────────────────────────────────────────────────────────────────────────────

bool parseIntLexeme(std::string_view lexeme, int64_t& out) {
    if (lexeme.empty()) return false;

    // ─── Radix detection ────────────────────────────────────────────────
    //
    // The lexeme begins with `0x`, `0b`, or `0o` for the non-decimal
    // radices, or a decimal digit for the decimal case. The radix is
    // detected from the prefix; the prefix is stripped before parsing.
    //
    // A leading `0` without a following letter is a decimal zero or a
    // decimal literal; it is not treated as an octal prefix (the
    // language has no C-style octal-by-leading-zero).
    int base = 10;
    std::string_view digits = lexeme;

    if (lexeme.size() >= 2 && lexeme[0] == '0') {
        switch (lexeme[1]) {
            case 'x': case 'X': base = 16; digits = lexeme.substr(2); break;
            case 'b': case 'B': base =  2; digits = lexeme.substr(2); break;
            case 'o': case 'O': base =  8; digits = lexeme.substr(2); break;
            default: /* decimal with a leading zero */ break;
        }
    }

    if (digits.empty()) return false;

    const char* begin = digits.data();
    const char* end   = digits.data() + digits.size();

    // `std::from_chars` with a base of 16 handles a leading `0x` only
    // if the pointer passed points past the `0x`. Since `digits` is
    // already stripped of the prefix, we pass the base directly.
    auto [ptr, ec] = std::from_chars(begin, end, out, base);
    if (ec != std::errc{} || ptr != end) return false;
    return true;
}

bool parseFloatLexeme(std::string_view lexeme, double& out) {
    if (lexeme.empty()) return false;

    // `std::from_chars` for `double` takes no radix; it parses the
    // lexical form `[-]d+.d+([eE][-+]?d+)?` that the grammar defines.
    const char* begin = lexeme.data();
    const char* end   = lexeme.data() + lexeme.size();

    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) return false;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// evaluateLiteral — the entry point
// ─────────────────────────────────────────────────────────────────────────────

ConstantValue evaluateLiteral(LiteralExprAST* lit, SemaContext& ctx) {
    if (!lit) return ConstantValue::unknown();

    const std::string_view lexeme = ctx.pool.lookupView(lit->value);

    switch (lit->kind) {
        // ─── Boolean literals ───────────────────────────────────────────
        case LiteralKind::True:
            return ConstantValue(true);
        case LiteralKind::False:
            return ConstantValue(false);

        // ─── Integer literals (all four radices) ────────────────────────
        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
        case LiteralKind::Octal: {
            int64_t value = 0;
            if (!parseIntLexeme(lexeme, value)) {
                // The parser accepts only well-formed integer lexemes;
                // reaching here means a compiler bug. Emit an internal
                // diagnostic and return Error so the caller does not
                // silently misread the literal.
                ctx.diagnostics.error(DiagCode::Value_IntegerOverflow, lit,
                                      "cannot parse integer literal '",
                                      std::string(lexeme), "'");
                return ConstantValue::error();
            }
            return ConstantValue(value);
        }

        // ─── Float literal ──────────────────────────────────────────────
        case LiteralKind::Float: {
            double value = 0.0;
            if (!parseFloatLexeme(lexeme, value)) {
                ctx.diagnostics.error(DiagCode::Value_NumericOverflow, lit,
                                      "cannot parse float literal '",
                                      std::string(lexeme), "'");
                return ConstantValue::error();
            }
            return ConstantValue(value);
        }

        // ─── String literals ────────────────────────────────────────────
        //
        // The parser has already stripped the surrounding quotes and
        // processed escape sequences. The lexeme on the `LiteralExprAST`
        // is the decoded text, interned. The evaluator returns it as a
        // `String` constant; a `Char` is the same representation with a
        // different `Kind`.
        case LiteralKind::String:
        case LiteralKind::RawString:
            return ConstantValue(lit->value);

        // ─── Char literal ───────────────────────────────────────────────
        case LiteralKind::Char: {
            ConstantValue result(lit->value);
            result.kind = ConstantValue::Kind::Char;
            return result;
        }

        // ─── Nil ────────────────────────────────────────────────────────
        case LiteralKind::Nil:
            return ConstantValue::nil();

        // ─── Unknown ────────────────────────────────────────────────────
        //
        // The parser never produces an `Unknown` literal kind. Reaching
        // here means a compiler bug.
        case LiteralKind::Unknown:
        default:
            AST_ASSERT_MSG(false,
                "evaluateLiteral: unrecognized LiteralKind");
            return ConstantValue::unknown();
    }
}

} // namespace lucid::sema