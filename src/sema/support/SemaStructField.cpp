/// @file SemaStructField.cpp
/// @brief Implementation of struct field analysis - focused on function field bodies.

#include "SemaStructField.hpp"
#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "../types/SemaLookup.hpp"
#include "../types/SemaResolve.hpp"
#include "../types/SemaCompare.hpp"
#include "debug/DebugUtils.hpp"

namespace sema {

namespace {

/// @brief Check if a NamedTypeAST refers to the same struct instantiation.
bool isSameStructInstantiation(const NamedTypeAST* named,
                                const StructDeclAST* currentStruct) {
    if (!named || !currentStruct) return false;

    if (named->name != currentStruct->name) return false;
    if (named->genericArgs.size() != currentStruct->genericParams.size()) {
        return false;
    }

    for (size_t i = 0; i < named->genericArgs.size(); ++i) {
        TypeAST* arg = named->genericArgs[i];
        const GenericParamDeclAST* param = currentStruct->genericParams[i];

        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* argNamed = arg->as<NamedTypeAST>();
            if (argNamed->name != param->name) {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

/// @brief Check if a field type is a self-reference.
SelfReferenceInfo checkSelfReferenceImpl(const TypeAST* fieldType,
                                          const StructDeclAST* currentStruct,
                                          SemaContext& ctx) {
    SelfReferenceInfo result;
    result.isSelfReference = false;

    if (!fieldType || !currentStruct) return result;

    bool isNullable = false;
    bool isPointer = false;
    const TypeAST* innerType = fieldType;

    if (fieldType->isa<NullableTypeAST>()) {
        isNullable = true;
        innerType = fieldType->as<NullableTypeAST>()->inner;
    }

    if (innerType->isa<PtrTypeAST>()) {
        isPointer = true;
        innerType = innerType->as<PtrTypeAST>()->inner;
    }

    if (!innerType->isa<NamedTypeAST>()) {
        return result;
    }

    const NamedTypeAST* named = innerType->as<NamedTypeAST>();

    if (named->name != currentStruct->name) {
        return result;
    }

    if (!isSameStructInstantiation(named, currentStruct)) {
        return result;
    }

    result.isSelfReference = true;
    result.isPointer = isPointer;
    result.isNullable = isNullable;
    result.isNonNullable = !isNullable && !isPointer;
    result.namedType = named;

    return result;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Self-Reference Detection (Public)
// ─────────────────────────────────────────────────────────────────────────────

SelfReferenceInfo checkSelfReference(const TypeAST* fieldType,
                                      const StructDeclAST* currentStruct,
                                      SemaContext& ctx) {
    return checkSelfReferenceImpl(fieldType, currentStruct, ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Function Field Body Analysis
// ─────────────────────────────────────────────────────────────────────────────

void analyzeFunctionFieldBody(const FieldDeclAST* field,
                               const StructDeclAST* currentStruct,
                               SemaContext& ctx) {
    if (!field || !field->type || !field->type->isa<FuncTypeAST>()) {
        return;
    }

    FuncTypeAST* funcType = field->type->as<FuncTypeAST>();

    if (funcType->params.empty()) {
        ctx.error(field, DiagCode::E3003,
                  "function field '", ctx.pool.lookup(field->name),
                  "' has no parameters (expected at least 'self' parameter)");
        return;
    }

    if (!field->defaultVal) {
        ctx.error(field, DiagCode::E3003,
                  "function field '", ctx.pool.lookup(field->name),
                  "' must have a body (function literal)");
        return;
    }

    // Push scope for function parameters
    ctx.pushScope();

    // Register self parameter (first parameter is always 'self')
    registerParamName(funcType->params[0], ctx);

    // Register the rest of the parameters
    for (size_t i = 1; i < funcType->params.size(); ++i) {
        registerParamName(funcType->params[i], ctx);
    }

    // Analyze the body
    if (field->defaultVal->isa<BlockStmtAST>()) {
        resolveBlock(field->defaultVal->as<BlockStmtAST>(), ctx);
    } else {
        // Expression bodies - resolve the expression
        resolveExpr(field->defaultVal, ctx);
        // TODO: Check that the expression is a function literal
    }

    ctx.popScope();
}

} // namespace sema