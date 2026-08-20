/// @file const_eval/ConstEvalUnary.cpp
/// @brief Unary operation evaluation for const expressions.
/// Only integer operations are evaluated at compile-time.
/// Float operations are left to LLVM's constant folding.

#include "ConstEvalHelpers.hpp"
#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"

#include <climits>

namespace sema {

// ─── Individual Unary Operations ──────────────────────────────────────

ConstantValue ConstEvaluator::evalNeg(SemaContext& ctx, const ConstantValue& operand,
                                       BaseAST* node, TypeAST* targetType) {
    if (operand.isInt()) {
        if (operand.asInt() == INT64_MIN) {
            ctx.diagnostics.error(DiagCode::Sem_IntegerOverflow, node,
                                  "integer overflow in const negation (INT64_MIN)");
            return ConstantValue::error();
        }
        return ConstantValue(-operand.asInt());
    }
    
    // Float negation: let LLVM handle it
    return ConstantValue::unknown();
}

ConstantValue ConstEvaluator::evalNot(SemaContext& ctx, const ConstantValue& operand,
                                       BaseAST* node) {
    if (operand.isBool()) {
        return ConstantValue(!operand.asBool());
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, node,
                          "logical not requires bool operand");
    return ConstantValue::error();
}

ConstantValue ConstEvaluator::evalBitNot(SemaContext& ctx, const ConstantValue& operand,
                                          BaseAST* node) {
    if (operand.isInt()) {
        return ConstantValue(~operand.asInt());
    }
    
    ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, node,
                          "bitwise not requires integer operand");
    return ConstantValue::error();
}

// ─── Unary Operation Evaluation ──────────────────────────────────────

ConstantValue ConstEvaluator::evalUnary(SemaContext& ctx, UnaryExprAST* expr,
                                         TypeAST* targetType) {
    ConstantValue operand = evaluate(ctx, expr->operand, targetType);
    if (operand.isError()) return operand;
    if (operand.isUnknown()) return ConstantValue::unknown();

    switch (expr->op) {
        case UnaryOp::Neg:
            return evalNeg(ctx, operand, expr, targetType);
        case UnaryOp::Not:
            return evalNot(ctx, operand, expr);
        case UnaryOp::BitNot:
            return evalBitNot(ctx, operand, expr);
        default:
            return ConstantValue::unknown();
    }
}

} // namespace sema