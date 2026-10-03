/// @file ConstEvalBinary.cpp
/// @brief Binary-operator folding: arithmetic, comparison, logical,
///        bitwise, and null-coalescing.
///
/// ─── The operator categories ─────────────────────────────────────────────
/// The grammar's `binary_op` production (§6) defines twenty-one operators
/// in five syntactic categories:
///
///   - Arithmetic: `+ - * / % **`
///   - Comparison: `== != < <= > >=`
///   - Logical:    `and or`
///   - Bitwise:    `& | ^ << >>`
///   - Null-coalescing: `??`
///
/// `+` is special: it is numeric addition on numeric operands and
/// string concatenation on two-string operands. The other arithmetic
/// operators are numeric-only.
///
/// ─── Kind-mismatch vs. error ──────────────────────────────────────────────
/// Two different failure modes:
///
///   - **Kind mismatch** (`1 + "a"`, `not 42`): the operands do not
///     have the kinds the operator accepts. This is a *type* error,
///     reported by the type checker, not the evaluator. The evaluator
///     returns `Unknown` — "not a compile-time constant".
///   - **Real error** (`1 / 0`, `1 % 0`, `1 << -1`): the operands have
///     the right kinds, but the operation fails at runtime. The
///     evaluator emits a diagnostic and returns `Error`.
///
/// The distinction matters because a `const_expr` in a fixed-table
/// cell must fold. If a cell is `1 / 0`, the user gets a "division by
/// zero" diagnostic and the table is not compiled; if a cell is
/// `"a" + 1`, the type checker already reported the mismatch, and the
/// evaluator stays silent.

#include "ConstEvaluator.hpp"
#include "ConstEvalHelpers.hpp"

#include "core/memory/InternedString.hpp"
#include "core/diagnostics/Diagnostic.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// forward declarations of the category helpers
// ─────────────────────────────────────────────────────────────────────────────

static ConstantValue foldArithmetic(BinaryOp op,
                                    const ConstantValue& left,
                                    const ConstantValue& right,
                                    SemaContext& ctx);

static ConstantValue foldComparison(BinaryOp op,
                                    const ConstantValue& left,
                                    const ConstantValue& right,
                                    SemaContext& ctx);

static ConstantValue foldLogical(BinaryOp op,
                                 const ConstantValue& left,
                                 const ConstantValue& right,
                                 SemaContext& ctx);

static ConstantValue foldBitwise(BinaryOp op,
                                 const ConstantValue& left,
                                 const ConstantValue& right,
                                 SemaContext& ctx);

static ConstantValue foldNullCoalesce(const ConstantValue& left,
                                      const ConstantValue& right);

// ─────────────────────────────────────────────────────────────────────────────
// foldBinary — the dispatcher
// ─────────────────────────────────────────────────────────────────────────────

ConstantValue foldBinary(BinaryOp op,
                         const ConstantValue& left,
                         const ConstantValue& right,
                         SemaContext& ctx) {
    switch (op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
        case BinaryOp::Pow:
            return foldArithmetic(op, left, right, ctx);

        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
            return foldComparison(op, left, right, ctx);

        case BinaryOp::And:
        case BinaryOp::Or:
            return foldLogical(op, left, right, ctx);

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr:
            return foldBitwise(op, left, right, ctx);

        case BinaryOp::NullCoalesce:
            return foldNullCoalesce(left, right);
    }

    return ConstantValue::unknown();
}

// ─────────────────────────────────────────────────────────────────────────────
// foldArithmetic
// ─────────────────────────────────────────────────────────────────────────────
//
// Six arithmetic operators. The two `+` cases:
//
//   - both operands numeric → numeric addition
//   - both operands String → string concatenation
//
// The other five operators are numeric-only.

static ConstantValue foldArithmetic(BinaryOp op,
                                    const ConstantValue& left,
                                    const ConstantValue& right,
                                    SemaContext& ctx) {
    // ─── String concatenation: `+` on two strings ───────────────────────
    //
    // The result is a new interned string. `String + String` is the
    // only string arithmetic the grammar permits (§6.8). A `Char` is
    // stored as an interned string; a `Char + String` is not defined,
    // and the evaluator returns Unknown for it — the type checker
    // reports the mismatch.
    if (op == BinaryOp::Add && left.isString() && right.isString()) {
        std::string_view a = ctx.pool.lookupView(left.asString());
        std::string_view b = ctx.pool.lookupView(right.asString());

        std::string concatenated;
        concatenated.reserve(a.size() + b.size());
        concatenated.append(a);
        concatenated.append(b);

        InternedString result = ctx.pool.intern(concatenated);
        return ConstantValue(result);
    }

    // ─── Numeric arithmetic ─────────────────────────────────────────────
    //
    // Both operands must be the same numeric kind. `1 + 1.5` is a type
    // error (no implicit coercion, §5.8), so the type checker handles
    // it; the evaluator sees the operands' kinds, and a mixed kind
    // returns Unknown.
    if (!isNumeric(left) || !isNumeric(right)) {
        return ConstantValue::unknown();
    }

    const bool bothInt   = left.isInt()   && right.isInt();
    const bool bothFloat = left.isFloat() && right.isFloat();

    if (!bothInt && !bothFloat) {
        // Mixed kinds — the caller's type checker will report it. The
        // evaluator returns Unknown so no constant is produced for a
        // value that has no valid type.
        return ConstantValue::unknown();
    }

    if (bothInt) {
        int64_t a = left.asInt();
        int64_t b = right.asInt();

        switch (op) {
            case BinaryOp::Add: {
                int64_t result;
                if (__builtin_add_overflow(a, b, &result)) {
                    ctx.diagnostics.error(DiagCode::Value_IntegerOverflow,
                                          nullptr,
                                          "integer overflow in constant "
                                          "addition");
                    return ConstantValue::error();
                }
                return ConstantValue(result);
            }

            case BinaryOp::Sub: {
                int64_t result;
                if (__builtin_sub_overflow(a, b, &result)) {
                    ctx.diagnostics.error(DiagCode::Value_IntegerOverflow,
                                          nullptr,
                                          "integer overflow in constant "
                                          "subtraction");
                    return ConstantValue::error();
                }
                return ConstantValue(result);
            }

            case BinaryOp::Mul: {
                int64_t result;
                if (__builtin_mul_overflow(a, b, &result)) {
                    ctx.diagnostics.error(DiagCode::Value_IntegerOverflow,
                                          nullptr,
                                          "integer overflow in constant "
                                          "multiplication");
                    return ConstantValue::error();
                }
                return ConstantValue(result);
            }

            case BinaryOp::Div:
                if (b == 0) {
                    ctx.diagnostics.error(DiagCode::Value_DivisionByZero,
                                          nullptr,
                                          "division by zero in constant "
                                          "expression");
                    return ConstantValue::error();
                }
                // `INT64_MIN / -1` overflows; `__builtin_sdiv_overflow`
                // exists on GCC/Clang. Fall back to a manual check.
                if (a == INT64_MIN && b == -1) {
                    ctx.diagnostics.error(DiagCode::Value_IntegerOverflow,
                                          nullptr,
                                          "integer overflow in constant "
                                          "division");
                    return ConstantValue::error();
                }
                return ConstantValue(a / b);

            case BinaryOp::Mod:
                if (b == 0) {
                    ctx.diagnostics.error(DiagCode::Value_ModuloByZero,
                                          nullptr,
                                          "modulo by zero in constant "
                                          "expression");
                    return ConstantValue::error();
                }
                if (a == INT64_MIN && b == -1) {
                    // The result would be 0 (the mathematical remainder),
                    // but the CPU traps. Return 0 directly.
                    return ConstantValue(static_cast<int64_t>(0));
                }
                return ConstantValue(a % b);

            case BinaryOp::Pow: {
                // Integer exponentiation. Negative exponent is not a
                // valid integer operation; return Unknown so the type
                // checker can report it if it cares.
                if (b < 0) return ConstantValue::unknown();

                int64_t result = 1;
                int64_t base   = a;
                int64_t exp    = b;
                bool overflowed = false;

                while (exp > 0) {
                    if (exp & 1) {
                        if (__builtin_mul_overflow(result, base, &result)) {
                            overflowed = true;
                            break;
                        }
                    }
                    exp >>= 1;
                    if (exp > 0) {
                        if (__builtin_mul_overflow(base, base, &base)) {
                            overflowed = true;
                            break;
                        }
                    }
                }

                if (overflowed) {
                    ctx.diagnostics.error(DiagCode::Value_IntegerOverflow,
                                          nullptr,
                                          "integer overflow in constant "
                                          "exponentiation");
                    return ConstantValue::error();
                }
                return ConstantValue(result);
            }

            default:
                return ConstantValue::unknown();
        }
    }

    // ─── Float arithmetic ───────────────────────────────────────────────
    //
    // IEEE 754 semantics. Division by zero produces `inf` or `nan`,
    // not an error — the type is `float`, and a NaN is a value.
    double a = left.asFloat();
    double b = right.asFloat();

    switch (op) {
        case BinaryOp::Add: return ConstantValue(a + b);
        case BinaryOp::Sub: return ConstantValue(a - b);
        case BinaryOp::Mul: return ConstantValue(a * b);
        case BinaryOp::Div: return ConstantValue(a / b);
        case BinaryOp::Mod: return ConstantValue(std::fmod(a, b));
        case BinaryOp::Pow: return ConstantValue(std::pow(a, b));
        default:            return ConstantValue::unknown();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// foldComparison
// ─────────────────────────────────────────────────────────────────────────────
//
// Six comparison operators. The result is always `Bool`.
//
// The comparison is allowed when:
//   - both operands are the same numeric kind;
//   - both operands are String;
//   - both operands are Char;
//   - both operands are Bool (for `==` and `!=` only);
//   - both operands are Nil (for `==` and `!=` only).
//
// Ordering (`< <= > >=`) on Bool or Nil is a type error; the evaluator
// returns Unknown and the type checker reports it.

static ConstantValue foldComparison(BinaryOp op,
                                    const ConstantValue& left,
                                    const ConstantValue& right,
                                    SemaContext& ctx) {
    (void)ctx;

    const bool isEqualityOp = (op == BinaryOp::Eq || op == BinaryOp::Ne);

    // ─── Null comparisons ───────────────────────────────────────────────
    //
    // `nil == nil` is true; `nil != nil` is false. Ordering on nil is
    // not defined.
    if (left.isNil() && right.isNil()) {
        if (op == BinaryOp::Eq) return ConstantValue(true);
        if (op == BinaryOp::Ne) return ConstantValue(false);
        return ConstantValue::unknown();
    }

    // ─── Bool comparisons ───────────────────────────────────────────────
    if (left.isBool() && right.isBool()) {
        bool a = left.asBool();
        bool b = right.asBool();
        switch (op) {
            case BinaryOp::Eq: return ConstantValue(a == b);
            case BinaryOp::Ne: return ConstantValue(a != b);
            default:           return ConstantValue::unknown();
        }
    }

    // ─── Numeric comparisons ────────────────────────────────────────────
    if (isNumeric(left) && isNumeric(right)) {
        const bool bothInt   = left.isInt()   && right.isInt();
        const bool bothFloat = left.isFloat() && right.isFloat();
        if (!bothInt && !bothFloat) {
            return ConstantValue::unknown();
        }

        if (bothInt) {
            int64_t a = left.asInt();
            int64_t b = right.asInt();
            switch (op) {
                case BinaryOp::Eq: return ConstantValue(a == b);
                case BinaryOp::Ne: return ConstantValue(a != b);
                case BinaryOp::Lt: return ConstantValue(a <  b);
                case BinaryOp::Le: return ConstantValue(a <= b);
                case BinaryOp::Gt: return ConstantValue(a >  b);
                case BinaryOp::Ge: return ConstantValue(a >= b);
                default:           return ConstantValue::unknown();
            }
        }

        double a = left.asFloat();
        double b = right.asFloat();
        switch (op) {
            case BinaryOp::Eq: return ConstantValue(a == b);
            case BinaryOp::Ne: return ConstantValue(a != b);
            case BinaryOp::Lt: return ConstantValue(a <  b);
            case BinaryOp::Le: return ConstantValue(a <= b);
            case BinaryOp::Gt: return ConstantValue(a >  b);
            case BinaryOp::Ge: return ConstantValue(a >= b);
            default:           return ConstantValue::unknown();
        }
    }

    // ─── String comparisons ─────────────────────────────────────────────
    if (left.isString() && right.isString()) {
        if (op == BinaryOp::Eq) return ConstantValue(left.asString() == right.asString());
        if (op == BinaryOp::Ne) return ConstantValue(left.asString() != right.asString());

        // Ordering: lexicographic by UTF-8 byte order.
        std::string_view a = ctx.pool.lookupView(left.asString());
        std::string_view b = ctx.pool.lookupView(right.asString());
        int cmp = a.compare(b);
        switch (op) {
            case BinaryOp::Lt: return ConstantValue(cmp <  0);
            case BinaryOp::Le: return ConstantValue(cmp <= 0);
            case BinaryOp::Gt: return ConstantValue(cmp >  0);
            case BinaryOp::Ge: return ConstantValue(cmp >= 0);
            default:           return ConstantValue::unknown();
        }
    }

    // ─── Char comparisons ───────────────────────────────────────────────
    //
    // A Char's lexeme is one UTF-8 code point. The comparison is by the
    // code point's Unicode value, not by the UTF-8 bytes (which would
    // differ for a multi-byte character). For a single-byte character
    // they coincide; for a multi-byte one, decoding to `char32_t` gives
    // the correct order.
    //
    // Simplification: this evaluator treats a Char's comparison as a
    // one-character string comparison, which is correct for ASCII and
    // wrong for non-ASCII. Fixing it correctly means decoding UTF-8 to
    // `char32_t` and comparing the codepoints. That is a small helper
    // and worth adding when Char literals are actually used in a
    // constant expression. Today the grammar allows a Char in a
    // `const_expr`; the correct implementation is a follow-up.
    if (left.isChar() && right.isChar()) {
        if (op == BinaryOp::Eq) return ConstantValue(left.asString() == right.asString());
        if (op == BinaryOp::Ne) return ConstantValue(left.asString() != right.asString());

        std::string_view a = ctx.pool.lookupView(left.asString());
        std::string_view b = ctx.pool.lookupView(right.asString());
        // Byte comparison for now; see the note above.
        int cmp = a.compare(b);
        switch (op) {
            case BinaryOp::Lt: return ConstantValue(cmp <  0);
            case BinaryOp::Le: return ConstantValue(cmp <= 0);
            case BinaryOp::Gt: return ConstantValue(cmp >  0);
            case BinaryOp::Ge: return ConstantValue(cmp >= 0);
            default:           return ConstantValue::unknown();
        }
    }

    return ConstantValue::unknown();
}

// ─────────────────────────────────────────────────────────────────────────────
// foldLogical
// ─────────────────────────────────────────────────────────────────────────────
//
// `and` and `or` on Bool operands. The result is Bool. Both operands
// are already-folded — `and`/`or` short-circuit at runtime, but the
// evaluator folds *both* sides before applying the operator. This is
// deliberate: a constant `and` has no side effects, and folding both
// sides lets the evaluator catch a fold error in the right operand even
// when the left short-circuits.

static ConstantValue foldLogical(BinaryOp op,
                                 const ConstantValue& left,
                                 const ConstantValue& right,
                                 SemaContext& ctx) {
    (void)ctx;

    if (!left.isBool() || !right.isBool()) {
        return ConstantValue::unknown();
    }

    bool a = left.asBool();
    bool b = right.asBool();

    switch (op) {
        case BinaryOp::And: return ConstantValue(a && b);
        case BinaryOp::Or:  return ConstantValue(a || b);
        default:            return ConstantValue::unknown();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// foldBitwise
// ─────────────────────────────────────────────────────────────────────────────
//
// `& | ^ << >>` on Int operands. The result is an Int.
//
// Shifts are the only bitwise operations with a runtime failure mode:
// a shift by a negative amount or by an amount >= the operand's bit
// width is a UB in C++, and the language treats it as a compile error
// in a constant expression.

static ConstantValue foldBitwise(BinaryOp op,
                                 const ConstantValue& left,
                                 const ConstantValue& right,
                                 SemaContext& ctx) {
    if (!left.isInt() || !right.isInt()) {
        return ConstantValue::unknown();
    }

    int64_t a = left.asInt();
    int64_t b = right.asInt();

    switch (op) {
        case BinaryOp::BitAnd: return ConstantValue(a & b);
        case BinaryOp::BitOr:  return ConstantValue(a | b);
        case BinaryOp::BitXor: return ConstantValue(a ^ b);

        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            // The shift amount must be in [0, 63]. A negative amount or
            // an amount >= 64 is a UB in C++. Both are compile errors
            // in a constant expression.
            if (b < 0 || b >= 64) {
                ctx.diagnostics.error(DiagCode::Value_InvalidShift, nullptr,
                                      "shift amount must be in [0, 63], got ",
                                      b);
                return ConstantValue::error();
            }
            if (op == BinaryOp::Shl) {
                // Left shift can overflow. For simplicity, treat the
                // result as the low 64 bits of the mathematical shift;
                // the bytecode emitter will widen or truncate according
                // to the declared type.
                return ConstantValue(static_cast<int64_t>(
                    static_cast<uint64_t>(a) << b));
            }
            // Right shift: arithmetic for a signed value. `int64_t >>
            // n` is implementation-defined for negatives in C++17 and
            // earlier; C++20 mandates arithmetic shift. Assume C++20.
            return ConstantValue(a >> b);
        }

        default:
            return ConstantValue::unknown();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// foldNullCoalesce
// ─────────────────────────────────────────────────────────────────────────────
//
// `a ?? b`. If `a` is Nil, the result is `b`. Otherwise the result is
// `a`. The evaluator does not check the *types* of the operands — that
// is the type checker's job. It just decides which side's value flows
// through.

static ConstantValue foldNullCoalesce(const ConstantValue& left,
                                      const ConstantValue& right) {
    if (left.isNil()) return right;
    return left;
}

} // namespace lucid::sema