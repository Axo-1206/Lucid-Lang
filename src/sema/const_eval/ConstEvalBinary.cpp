/// @file const_eval/ConstEvalBinary.cpp
/// @brief Binary operation evaluation for const expressions.

#include "ConstEvalHelpers.hpp"
#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"

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
        case ConstantValue::Kind::Float:
            return a.asFloat() == b.asFloat();
        case ConstantValue::Kind::String:
        case ConstantValue::Kind::Char:
            return ctx.pool.lookup(a.asString()) == ctx.pool.lookup(b.asString());
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
            // Compare directly rather than subtracting — a.asInt() -
            // b.asInt() itself overflows int64_t for sufficiently
            // far-apart values (e.g. INT64_MAX vs INT64_MIN), the same
            // class of bug evalDiv/evalMod's INT64_MIN/-1 case is.
            int64_t l = a.asInt();
            int64_t r = b.asInt();
            return (l > r) ? 1 : (l < r) ? -1 : 0;
        }
        case ConstantValue::Kind::Float: {
            double diff = a.asFloat() - b.asFloat();
            return (diff > 0) ? 1 : (diff < 0) ? -1 : 0;
        }
        case ConstantValue::Kind::String:
        case ConstantValue::Kind::Char:
            return ctx.pool.lookup(a.asString()).compare(ctx.pool.lookup(b.asString()));
        default:
            return 0;
    }
}

// ─── Individual Arithmetic Operations ─────────────────────────────────

ConstantValue ConstEvaluator::evalAdd(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       const BaseAST* node, const TypeAST* targetType) {
    // Integer + Integer
    if (left.isInt() && right.isInt()) {
        int64_t l = left.asInt();
        int64_t r = right.asInt();
        if ((r > 0 && l > INT64_MAX - r) || (r < 0 && l < INT64_MIN - r)) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const addition");
            return ConstantValue::error();
        }
        return ConstantValue(l + r);
    }
    
    // Float + Float
    if (left.isFloat() && right.isFloat()) {
        return ConstantValue(left.asFloat() + right.asFloat());
    }
    
    // Integer + Float (promote int to float)
    if (left.isInt() && right.isFloat()) {
        return ConstantValue(static_cast<double>(left.asInt()) + right.asFloat());
    }
    if (left.isFloat() && right.isInt()) {
        return ConstantValue(left.asFloat() + static_cast<double>(right.asInt()));
    }
    
    // String concatenation
    if (left.isString() && right.isString()) {
        std::string result = ctx.pool.lookup(left.asString());
        result += ctx.pool.lookup(right.asString());
        return ConstantValue(ctx.pool.intern(result));
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                          "invalid operands for '+'");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalSub(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       const BaseAST* node, const TypeAST* targetType) {
    // Integer - Integer
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
    
    // Float - Float
    if (left.isFloat() && right.isFloat()) {
        return ConstantValue(left.asFloat() - right.asFloat());
    }
    
    // Integer - Float (promote int to float)
    if (left.isInt() && right.isFloat()) {
        return ConstantValue(static_cast<double>(left.asInt()) - right.asFloat());
    }
    if (left.isFloat() && right.isInt()) {
        return ConstantValue(left.asFloat() - static_cast<double>(right.asInt()));
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                          "invalid operands for '-'");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalMul(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       const BaseAST* node, const TypeAST* targetType) {
    // Integer * Integer
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
    
    // Float * Float
    if (left.isFloat() && right.isFloat()) {
        return ConstantValue(left.asFloat() * right.asFloat());
    }
    
    // Integer * Float (promote int to float)
    if (left.isInt() && right.isFloat()) {
        return ConstantValue(static_cast<double>(left.asInt()) * right.asFloat());
    }
    if (left.isFloat() && right.isInt()) {
        return ConstantValue(left.asFloat() * static_cast<double>(right.asInt()));
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                          "invalid operands for '*'");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalDiv(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       const BaseAST* node, const TypeAST* targetType) {
    // Integer / Integer
    if (left.isInt() && right.isInt()) {
        if (right.asInt() == 0) {
            return handleArithmeticError(ctx, "/", "division by zero", node, targetType);
        }
        // INT64_MIN / -1 is undefined behavior in C++ — the mathematical
        // result (INT64_MAX + 1) doesn't fit in int64_t. Same overflow
        // class as evalAdd/evalSub/evalMul/evalNeg already guard against.
        if (left.asInt() == INT64_MIN && right.asInt() == -1) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const division (INT64_MIN / -1)");
            return ConstantValue::error();
        }
        return ConstantValue(left.asInt() / right.asInt());
    }
    
    // Float / Float
    if (left.isFloat() && right.isFloat()) {
        if (right.asFloat() == 0.0) {
            return handleArithmeticError(ctx, "/", "division by zero", node, targetType);
        }
        return ConstantValue(left.asFloat() / right.asFloat());
    }
    
    // Integer / Float (promote int to float)
    if (left.isInt() && right.isFloat()) {
        if (right.asFloat() == 0.0) {
            return handleArithmeticError(ctx, "/", "division by zero", node, targetType);
        }
        return ConstantValue(static_cast<double>(left.asInt()) / right.asFloat());
    }
    if (left.isFloat() && right.isInt()) {
        if (right.asInt() == 0) {
            return handleArithmeticError(ctx, "/", "division by zero", node, targetType);
        }
        return ConstantValue(left.asFloat() / static_cast<double>(right.asInt()));
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                          "invalid operands for '/'");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalMod(SemaContext& ctx, const ConstantValue& left, const ConstantValue& right,
                                       const BaseAST* node, const TypeAST* targetType) {
    if (left.isInt() && right.isInt()) {
        if (right.asInt() == 0) {
            return handleArithmeticError(ctx, "%", "modulo by zero", node, targetType);
        }
        // Same UB case as evalDiv, above: INT64_MIN % -1 is undefined
        // behavior in C++ even though the mathematical result is 0.
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
                                       const BaseAST* node, const TypeAST* targetType) {
    if (bothNumeric(left, right)) {
        double base = toDouble(left);
        double exp = toDouble(right);
        if (base == 0.0 && exp < 0) {
            return handleArithmeticError(ctx, "**", "0 raised to negative power", node, targetType);
        }
        return ConstantValue(std::pow(base, exp));
    }
    ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, node,
                          "pow requires numeric operands");
    return ConstantValue::error();
}

// ─── Binary Operation Evaluation ──────────────────────────────────────

ConstantValue ConstEvaluator::evalBinaryOp(SemaContext& ctx, BinaryOp op,
                                            const ConstantValue& left,
                                            const ConstantValue& right,
                                            const BaseAST* node,
                                            const TypeAST* targetType) {
    switch (op) {
        // ─── Arithmetic ──────────────────────────────────────────────────
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

        // ─── Comparison ──────────────────────────────────────────────────
        case BinaryOp::Eq:
            return ConstantValue(compareEqual(ctx, left, right));
        case BinaryOp::Ne:
            return ConstantValue(!compareEqual(ctx, left, right));
        case BinaryOp::Lt:
            return ConstantValue(compareOrder(ctx, left, right) < 0);
        case BinaryOp::Gt:
            return ConstantValue(compareOrder(ctx, left, right) > 0);
        case BinaryOp::Le:
            return ConstantValue(compareOrder(ctx, left, right) <= 0);
        case BinaryOp::Ge:
            return ConstantValue(compareOrder(ctx, left, right) >= 0);

        // ─── Logical ──────────────────────────────────────────────────────
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

        // ─── Bitwise ──────────────────────────────────────────────────────
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
                                          "shift amount exceeds bit width");
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