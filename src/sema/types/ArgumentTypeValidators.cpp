/// @file registry/ArgumentTypeValidators.cpp
/// @brief Implementation of argument type validation utilities.

#include "ArgumentTypeValidators.hpp"
#include "../types/SemaCompare.hpp"
#include "debug/DebugUtils.hpp"
#include "sema/Sema.hpp"

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
    if (!arg) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, nullptr,
                              "argument '", argName, "' is null");
        return false;
    }

    TypeAST* result = resolveExprWithTarget(
        arg, ctx.getStringType(), ctx
    );
    if (!result || result->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                              "argument '", argName, "' expects a string literal");
        return false;
    }

    if (!arg->isa<LiteralExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                              "argument '", argName, "' must be a string literal (not an expression)");
        return false;
    }

    LiteralExprAST* lit = arg->as<LiteralExprAST>();
    if (lit->kind != LiteralKind::String && lit->kind != LiteralKind::RawString) {
        ctx.diagnostics.error(DiagCode::Sem_AttributeArgValue, arg,
                              "argument '", argName, "' must be a string literal");
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