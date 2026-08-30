#pragma once

#include "../const_eval/ConstEvaluator.hpp"
#include "../types/SemaType.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

namespace sema {

/// Return the truth value of a successfully evaluated constant.
inline bool constantTruthiness(const ConstantValue& value, SemaContext& ctx) {
    if (value.isBool()) return value.asBool();
    if (value.isInt()) return value.asInt() != 0;
    if (value.isFloat()) return value.asFloat() != 0.0;
    if (value.isString()) return !ctx.pool.lookup(value.asString()).empty();
    if (value.isArray()) return !value.asArray().empty();
    if (value.isNil() || value.isErr()) return false;

    // Chars, enums, structs, and other concrete values are truthy.
    return value.isChar() || value.isEnum() || value.isStruct() || value.isFunction();
}

/// Evaluate an expression's truthiness when it is known at compile time.
/// Returns false when the expression cannot be evaluated; callers must check
/// `isConstTruthiness` when they need to distinguish that case.
inline bool evaluateTruthiness(ExprAST* expr, SemaContext& ctx,
                               bool& isConstTruthiness) {
    isConstTruthiness = false;
    if (!expr) return false;

    ConstantValue value = ConstEvaluator::getConstValue(ctx, expr);
    if (value.isError() || value.isUnknown() || value.isVoid()) return false;

    isConstTruthiness = value.isEvaluated();
    return isConstTruthiness ? constantTruthiness(value, ctx) : false;
}

} // namespace sema
