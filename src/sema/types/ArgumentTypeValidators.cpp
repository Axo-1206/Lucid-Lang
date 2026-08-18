/// @file registry/ArgumentTypeValidators.cpp
/// @brief Implementation of argument type validation utilities.

#include "ArgumentTypeValidators.hpp"
#include "../types/SemaCompare.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

// ─── Argument Type Validators ─────────────────────────────────────────────

bool validatePtrArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !arg->resolvedType->isa<PtrTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects pointer type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateNumericArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isNumericType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects numeric type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateIntArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isIntegerType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects integer type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateStringArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isStringType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects string type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateBoolArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !isBoolType(arg->resolvedType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects boolean type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

bool validateRefArg(ExprAST* arg, const std::string& argName, SemaContext& ctx) {
    if (!arg->resolvedType || !arg->resolvedType->isa<RefTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects reference type, got ",
                              debug::typeToString(arg->resolvedType, ctx.pool));
        return false;
    }
    return true;
}

} // namespace sema