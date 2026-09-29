/// @file ConstEvalUnary.cpp
/// @brief Unary-operator folding: `-`, `not`, `~`.
///
/// Three operators, three narrow domains:
///
///   - `-` (Neg) requires a numeric operand (Int or Float); the result
///     is the negation in the same kind.
///   - `not` (Not) requires a Bool operand; the result is its logical
///     negation.
///   - `~` (BitNot) requires an Int operand; the result is its bitwise
///     complement.
///
/// Anything else is a kind mismatch. A kind mismatch is not an error —
/// the evaluator returns `Unknown`, which the caller interprets as "not
/// a compile-time constant". The type checker, not the evaluator, is
/// responsible for reporting a genuine type error like `not 42`.

#include "ConstEvaluator.hpp"
#include "ConstEvalHelpers.hpp"

#include "sema/context/SemaContext.hpp"

namespace lucid::sema {

ConstantValue foldUnary(UnaryOp op, const ConstantValue& operand,
                        SemaContext& ctx) {
    (void)ctx;   // No diagnostics are emitted on a kind mismatch.

    switch (op) {
        // ─── Negation ───────────────────────────────────────────────────
        case UnaryOp::Neg: {
            if (operand.isInt()) {
                return ConstantValue(-operand.asInt());
            }
            if (operand.isFloat()) {
                return ConstantValue(-operand.asFloat());
            }
            return ConstantValue::unknown();
        }

        // ─── Logical not ────────────────────────────────────────────────
        case UnaryOp::Not: {
            if (operand.isBool()) {
                return ConstantValue(!operand.asBool());
            }
            return ConstantValue::unknown();
        }

        // ─── Bitwise not ────────────────────────────────────────────────
        //
        // The value is stored as an `int64_t`. `~v` is the same
        // regardless of the operand's declared width; the width matters
        // at the bytecode's storage site, not in the fold.
        case UnaryOp::BitNot: {
            if (operand.isInt()) {
                return ConstantValue(~operand.asInt());
            }
            return ConstantValue::unknown();
        }
    }

    return ConstantValue::unknown();
}

} // namespace lucid::sema