/**
 * @file TypeChecker.cpp
 * @brief Implementation of type checking utilities.
 */

#include "TypeChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolver/TypeResolver.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"

namespace luc {

// ============================================================================
// Equality & Assignment
// ============================================================================

bool TypeChecker::isEqual(TypeAST* a, TypeAST* b, TypeResolver& resolver) {
    return resolver.typesEqual(a, b);
}

bool TypeChecker::isAssignable(TypeAST* source, TypeAST* target,
                                SemanticContext& ctx) {
    if (!source || !target) return false;
    
    // Exact match after alias resolution
    if (ctx.typeResolver->typesEqual(source, target)) {
        return true;
    }
    
    // Nil assignment to nullable
    if (isNilLiteral(nullptr)) {  // Need to check actual expression
        if (isNullable(target, *ctx.typeResolver)) {
            return true;
        }
    }
    
    // Numeric promotion
    if (canPromote(source, target, *ctx.typeResolver)) {
        return true;
    }
    
    // Implicit conversion via from declaration
    if (canConvert(source, target, ctx)) {
        return true;
    }
    
    return false;
}

bool TypeChecker::canPromote(TypeAST* source, TypeAST* target,
                              TypeResolver& resolver) {
    int sourceRank = getNumericRank(source, resolver);
    int targetRank = getNumericRank(target, resolver);
    
    if (sourceRank <= 0 || targetRank <= 0) return false;
    
    // Promotion: source must be narrower than target
    return sourceRank < targetRank;
}

bool TypeChecker::canConvert(TypeAST* source, TypeAST* target,
                              SemanticContext& ctx) {
    // TODO: Look up from declarations
    // This requires a map from (source, target) -> conversion function
    // For now, return false
    return false;
}

// ============================================================================
// Numeric Operations
// ============================================================================

int TypeChecker::getNumericRank(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (!type) return -1;
    
    if (auto* prim = type->as<PrimitiveTypeAST>()) {
        switch (prim->primitiveKind) {
            case PrimitiveKind::Byte:
            case PrimitiveKind::Ubyte:
                return 1;
            case PrimitiveKind::Short:
            case PrimitiveKind::Ushort:
                return 2;
            case PrimitiveKind::Int:
            case PrimitiveKind::Uint:
                return 3;
            case PrimitiveKind::Long:
            case PrimitiveKind::Ulong:
                return 4;
            case PrimitiveKind::Float:
                return 5;
            case PrimitiveKind::Double:
                return 6;
            case PrimitiveKind::Decimal:
                return 7;
            default:
                return -1;
        }
    }
    return -1;
}

bool TypeChecker::isNumeric(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* prim = type->as<PrimitiveTypeAST>()) {
        switch (prim->primitiveKind) {
            case PrimitiveKind::Byte:
            case PrimitiveKind::Short:
            case PrimitiveKind::Int:
            case PrimitiveKind::Long:
            case PrimitiveKind::Ubyte:
            case PrimitiveKind::Ushort:
            case PrimitiveKind::Uint:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool TypeChecker::isInteger(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* prim = type->as<PrimitiveTypeAST>()) {
        switch (prim->primitiveKind) {
            case PrimitiveKind::Byte:
            case PrimitiveKind::Short:
            case PrimitiveKind::Int:
            case PrimitiveKind::Long:
            case PrimitiveKind::Ubyte:
            case PrimitiveKind::Ushort:
            case PrimitiveKind::Uint:
            case PrimitiveKind::Ulong:
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool TypeChecker::isFloat(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* prim = type->as<PrimitiveTypeAST>()) {
        switch (prim->primitiveKind) {
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal:
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool TypeChecker::isBoolean(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* prim = type->as<PrimitiveTypeAST>()) {
        return prim->primitiveKind == PrimitiveKind::Bool;
    }
    return false;
}

// ============================================================================
// Callable Validation
// ============================================================================

bool TypeChecker::isCallable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<FuncTypeAST>();
}

TypeAST* TypeChecker::getReturnType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* func = type->as<FuncTypeAST>()) {
        if (!func->returnTypes.empty()) {
            return func->returnTypes[0];
        }
    }
    return nullptr;
}

std::vector<TypeAST*> TypeChecker::getParameterTypes(TypeAST* type,
                                                      TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    std::vector<TypeAST*> params;
    
    if (auto* func = type->as<FuncTypeAST>()) {
        for (auto* param : func->params) {
            params.push_back(param->type);
        }
    }
    
    return params;
}

// ============================================================================
// Array Operations
// ============================================================================

bool TypeChecker::isArray(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<ArrayTypeAST>();
}

TypeAST* TypeChecker::getElementType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* array = type->as<ArrayTypeAST>()) {
        return array->element;
    }
    return nullptr;
}

bool TypeChecker::isSliceable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* array = type->as<ArrayTypeAST>()) {
        return true;  // All array kinds are sliceable
    }
    return false;
}

// ============================================================================
// Nullable Operations
// ============================================================================

bool TypeChecker::isNullable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<NullableTypeAST>();
}

TypeAST* TypeChecker::unwrapNullable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* nullable = type->as<NullableTypeAST>()) {
        return nullable->inner;
    }
    return type;
}

bool TypeChecker::isNilLiteral(ExprAST* expr) {
    if (auto* literal = expr->as<LiteralExprAST>()) {
        return literal->kind == LiteralKind::Nil;
    }
    return false;
}

// ============================================================================
// Result Type Operations
// ============================================================================

bool TypeChecker::isResult(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<ResultTypeAST>();
}

TypeAST* TypeChecker::getSuccessType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* result = type->as<ResultTypeAST>()) {
        return result->inner;
    }
    return nullptr;
}

TypeAST* TypeChecker::getErrorType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* result = type->as<ResultTypeAST>()) {
        return result->errorType;
    }
    return nullptr;
}

// ============================================================================
// Type Inference
// ============================================================================

TypeAST* TypeChecker::unify(TypeAST* a, TypeAST* b, SemanticContext& ctx) {
    if (!a || !b) return nullptr;
    
    // If they're already equal, return either
    if (ctx.typeResolver->typesEqual(a, b)) {
        return a;
    }
    
    // Numeric unification: find common numeric type
    if (isNumeric(a, *ctx.typeResolver) && isNumeric(b, *ctx.typeResolver)) {
        int rankA = getNumericRank(a, *ctx.typeResolver);
        int rankB = getNumericRank(b, *ctx.typeResolver);
        
        if (rankA >= rankB) return a;
        else return b;
    }
    
    return nullptr;
}

TypeAST* TypeChecker::commonType(TypeAST* a, TypeAST* b, SemanticContext& ctx) {
    // For binary operations, use unify
    return unify(a, b, ctx);
}

// ============================================================================
// Constant Evaluation
// ============================================================================

std::optional<int64_t> TypeChecker::getConstantIntValue(ExprAST* expr,
                                                         TypeResolver& resolver) {
    if (!expr) return std::nullopt;
    
    // Check if expression is marked as constant
    if (!expr->isConst) return std::nullopt;
    
    // Handle literal expressions
    if (auto* literal = expr->as<LiteralExprAST>()) {
        if (literal->kind == LiteralKind::Int) {
            // Parse integer value
            std::string_view val = resolver.ctx().pool.lookup(literal->value);
            return std::stoll(std::string(val));
        }
    }
    
    // TODO: Handle constant folding for binary operations, etc.
    
    return std::nullopt;
}

// ============================================================================
// Type Queries
// ============================================================================

TypeAST* TypeChecker::getUnderlyingType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    
    // Unwrap nullable
    if (isNullable(type, resolver)) {
        type = unwrapNullable(type, resolver);
    }
    
    return type;
}

bool TypeChecker::isVoid(TypeAST* type) {
    return type == nullptr;
}

// ============================================================================
// Private Helpers
// ============================================================================

bool TypeChecker::typesMatchForAssignment(TypeAST* source, TypeAST* target,
                                           TypeResolver& resolver) {
    if (!source || !target) return false;
    
    source = resolver.unwrapAlias(source);
    target = resolver.unwrapAlias(target);
    
    // Pointer equality after unwrapping
    if (source == target) return true;
    
    // Type kind check
    if (source->kind != target->kind) return false;
    
    // Recursive check for composite types
    // This is handled by TypeResolver::typesEqual
    return resolver.typesEqual(source, target);
}

} // namespace luc