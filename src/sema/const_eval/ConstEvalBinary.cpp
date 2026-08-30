/// @file const_eval/ConstEvalBinary.cpp
/// @brief Binary operation evaluation for const expressions.
///
/// Only integer operations are evaluated at compile-time because Sema needs
/// them for array sizes, enum discriminants, and control flow folding.
/// Floating-point and string operations are left to LLVM's constant folding.

#include "ConstEvalHelpers.hpp"
#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaType.hpp"

#include <cmath>
#include <climits>

namespace sema {

// ─── Binary Operation Helpers ──────────────────────────────────────────

/// @brief Compare two constant values for equality.
bool ConstEvaluator::compareEqual(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b) {
    if (a.kind != b.kind) return false;

    switch (a.kind) {
        case ConstantValue::Kind::Bool:
            return a.asBool() == b.asBool();
        case ConstantValue::Kind::Int:
            return a.asInt() == b.asInt();
        case ConstantValue::Kind::Nil:
        case ConstantValue::Kind::Err:
            return true;
        default:
            return false;
    }
}

/// @brief Compare two constant values for ordering.
int ConstEvaluator::compareOrder(SemaContext& ctx, const ConstantValue& a, const ConstantValue& b) {
    if (a.kind != b.kind) return 0;

    switch (a.kind) {
        case ConstantValue::Kind::Int: {
            int64_t l = a.asInt();
            int64_t r = b.asInt();
            return (l > r) ? 1 : (l < r) ? -1 : 0;
        }
        default:
            return 0;
    }
}

// ─── Individual Arithmetic Operations ─────────────────────────────────

ConstantValue ConstEvaluator::evalAdd(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       BaseAST* node, TypeAST* targetType) {
    // Only integer + integer is evaluated at compile time.
    // Float and string concatenation are left to LLVM.
    if (left.isInt() && right.isInt()) {
        int64_t l = left.asInt();
        int64_t r = right.asInt();
        // Check for overflow
        if ((r > 0 && l > INT64_MAX - r) || (r < 0 && l < INT64_MIN - r)) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const addition");
            return ConstantValue::error();
        }
        return ConstantValue(l + r);
    }
    
    // Float and string operations: let LLVM handle them
    return ConstantValue::unknown();
}

ConstantValue ConstEvaluator::evalSub(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       BaseAST* node, TypeAST* targetType) {
    if (left.isInt() && right.isInt()) {
        int64_t l = left.asInt();
        int64_t r = right.asInt();
        if ((r > 0 && l < INT64_MIN + r) || (r < 0 && l > INT64_MAX + r)) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const subtraction");
            return ConstantValue::error();
        }
        return ConstantValue(l - r);
    }
    
    return ConstantValue::unknown();
}

ConstantValue ConstEvaluator::evalMul(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       BaseAST* node, TypeAST* targetType) {
    if (left.isInt() && right.isInt()) {
        int64_t l = left.asInt();
        int64_t r = right.asInt();
        if (l != 0 && r != 0) {
            if ((r > 0 && (l > INT64_MAX / r || l < INT64_MIN / r)) ||
                (r < 0 && (l > INT64_MIN / -r || l < INT64_MAX / -r))) {
                ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                      "integer overflow in const multiplication");
                return ConstantValue::error();
            }
        }
        return ConstantValue(l * r);
    }
    
    return ConstantValue::unknown();
}

ConstantValue ConstEvaluator::evalDiv(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       BaseAST* node, TypeAST* targetType) {
    if (left.isInt() && right.isInt()) {
        if (right.asInt() == 0) {
            return handleArithmeticError(ctx, "/", "division by zero", node, targetType);
        }
        // INT64_MIN / -1 is undefined behavior
        if (left.asInt() == INT64_MIN && right.asInt() == -1) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const division (INT64_MIN / -1)");
            return ConstantValue::error();
        }
        return ConstantValue(left.asInt() / right.asInt());
    }
    
    return ConstantValue::unknown();
}

ConstantValue ConstEvaluator::evalMod(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       BaseAST* node, TypeAST* targetType) {
    if (left.isInt() && right.isInt()) {
        if (right.asInt() == 0) {
            return handleArithmeticError(ctx, "%", "modulo by zero", node, targetType);
        }
        if (left.asInt() == INT64_MIN && right.asInt() == -1) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const modulo (INT64_MIN % -1)");
            return ConstantValue::error();
        }
        return ConstantValue(left.asInt() % right.asInt());
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                          "modulo requires integer operands");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalPow(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       BaseAST* node, TypeAST* targetType) {
    // Only integer exponentiation with non-negative exponent
    if (left.isInt() && right.isInt()) {
        int64_t base = left.asInt();
        int64_t exp = right.asInt();
        
        if (exp < 0) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                                  "negative exponent in const integer power");
            return ConstantValue::error();
        }
        
        if (exp > 20) {  // Prevent huge results
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer exponent too large in const power (max 20)");
            return ConstantValue::error();
        }
        
        int64_t result = 1;
        for (int64_t i = 0; i < exp; ++i) {
            // Check overflow
            if (result > INT64_MAX / base) {
                ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                      "integer overflow in const power");
                return ConstantValue::error();
            }
            result *= base;
        }
        return ConstantValue(result);
    }
    
    // Float pow: let LLVM handle it
    return ConstantValue::unknown();
}

// ─── Binary Operation Evaluation ──────────────────────────────────────

ConstantValue ConstEvaluator::evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                            const ConstantValue& left,
                                            const ConstantValue& right,
                                            BaseAST* node,
                                            TypeAST* targetType) {
    switch (op) {
        // ─── Integer Arithmetic (Keep) ────────────────────────────────────
        case BinaryOp::Add:
            return evalAdd(ctx, left, right, node, targetType);
        case BinaryOp::Sub:
            return evalSub(ctx, left, right, node, targetType);
        case BinaryOp::Mul:
            return evalMul(ctx, left, right, node, targetType);
        case BinaryOp::Div:
            return evalDiv(ctx, left, right, node, targetType);
        case BinaryOp::Mod:
            return evalMod(ctx, left, right, node, targetType);
        case BinaryOp::Pow:
            return evalPow(ctx, left, right, node, targetType);

        // ─── Integer Comparisons (Keep) ──────────────────────────────────
        case BinaryOp::Eq:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(compareEqual(ctx, left, right));
            }
            return ConstantValue::unknown();  // Let LLVM handle float comparisons
            
        case BinaryOp::Ne:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(!compareEqual(ctx, left, right));
            }
            return ConstantValue::unknown();
            
        case BinaryOp::Lt:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(compareOrder(ctx, left, right) < 0);
            }
            return ConstantValue::unknown();
            
        case BinaryOp::Gt:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(compareOrder(ctx, left, right) > 0);
            }
            return ConstantValue::unknown();
            
        case BinaryOp::Le:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(compareOrder(ctx, left, right) <= 0);
            }
            return ConstantValue::unknown();
            
        case BinaryOp::Ge:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(compareOrder(ctx, left, right) >= 0);
            }
            return ConstantValue::unknown();

        // ─── Logical (Keep - used for if/while folding) ──────────────────
        case BinaryOp::And:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() && right.asBool());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, node,
                                  "'and' requires bool operands");
            return ConstantValue::error();

        case BinaryOp::Or:
            if (left.isBool() && right.isBool()) {
                return ConstantValue(left.asBool() || right.asBool());
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, node,
                                  "'or' requires bool operands");
            return ConstantValue::error();

        // ─── Integer Bitwise (Keep) ──────────────────────────────────────
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
            if (left.isInt() && right.isInt()) {
                return ConstantValue(
                    op == BinaryOp::BitAnd ? left.asInt() & right.asInt() :
                    op == BinaryOp::BitOr  ? left.asInt() | right.asInt() :
                                              left.asInt() ^ right.asInt()
                );
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, node,
                                  "bitwise operator requires integer operands");
            return ConstantValue::error();

        case BinaryOp::Shl:
        case BinaryOp::Shr:
            if (left.isInt() && right.isInt()) {
                if (right.asInt() < 0) {
                    ctx.diagnostics.error(DiagCode::Sem_NegativeShift, node,
                                          "negative shift amount in const expression");
                    return ConstantValue::error();
                }
                if (right.asInt() >= 64) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                          "shift amount exceeds bit width (max 63)");
                    return ConstantValue::error();
                }
                return ConstantValue(
                    op == BinaryOp::Shl ? left.asInt() << right.asInt() :
                                          left.asInt() >> right.asInt()
                );
            }
            ctx.diagnostics.error(DiagCode::Sem_InvalidShift, node,
                                  "shift requires integer operands");
            return ConstantValue::error();

        default:
            return ConstantValue::unknown();
    }
}

} // namespace sema