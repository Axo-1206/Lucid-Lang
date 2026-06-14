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

namespace TypeChecker {

// ============================================================================
// Private Helpers
// ============================================================================

namespace {
    /**
     * @brief Gets the numeric promotion rank of a type.
     * 
     * Higher rank = wider type.
     * Ranks: byte/ubyte(1) < short/ushort(2) < int/uint(3) < long/ulong(4) < float(5) < double(6) < decimal(7)
     * 
     * @param type The type to check
     * @param resolver Type resolver for alias unwrapping
     * @return int The rank (1-7), or -1 if not numeric
     */
    int getNumericRank(TypeAST* type, TypeResolver& resolver) {
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
    
    /**
     * @brief Checks if two types match for assignment after unwrapping.
     * 
     * @param source Source type
     * @param target Target type
     * @param resolver Type resolver for alias unwrapping
     * @return true if types match
     */
    bool typesMatchForAssignment(TypeAST* source, TypeAST* target, TypeResolver& resolver) {
        if (!source || !target) return false;
        
        source = resolver.unwrapAlias(source);
        target = resolver.unwrapAlias(target);
        
        if (source == target) return true;
        if (source->kind != target->kind) return false;
        
        return resolver.typesEqual(source, target);
    }
} // anonymous namespace

// ============================================================================
// Equality & Assignment
// ============================================================================

bool isEqual(TypeAST* a, TypeAST* b, TypeResolver& resolver) {
    return resolver.typesEqual(a, b);
}

bool typesEqualForComparison(TypeAST* a, TypeAST* b, TypeResolver& resolver) {
    if (!a || !b) return false;
    a = resolver.unwrapAlias(a);
    b = resolver.unwrapAlias(b);
    if (isEqual(a, b, resolver)) return true;
    if (isNumeric(a, resolver) && isNumeric(b, resolver)) return true;
    return false;
}

bool isAssignable(TypeAST* source, TypeAST* target, SemanticContext& ctx) {
    if (!source || !target) return false;
    
    // Exact match after alias resolution
    if (ctx.typeResolver->typesEqual(source, target)) {
        return true;
    }
    
    // Nil assignment to nullable
    // Note: This check requires the actual expression, which isn't available here
    // The caller should check isNilLiteral before calling isAssignable
    
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

bool canPromote(TypeAST* source, TypeAST* target, TypeResolver& resolver) {
    int sourceRank = getNumericRank(source, resolver);
    int targetRank = getNumericRank(target, resolver);
    
    if (sourceRank <= 0 || targetRank <= 0) return false;
    
    // Promotion: source must be narrower than target
    return sourceRank < targetRank;
}

bool canConvert(TypeAST* source, TypeAST* target, SemanticContext& ctx) {
    // TODO: Look up from declarations
    // This requires a map from (source, target) -> conversion function
    // For now, return false
    (void)source;
    (void)target;
    (void)ctx;
    return false;
}

// ============================================================================
// Numeric Operations
// ============================================================================

bool isNumeric(TypeAST* type, TypeResolver& resolver) {
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

bool isInteger(TypeAST* type, TypeResolver& resolver) {
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

bool isFloat(TypeAST* type, TypeResolver& resolver) {
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

bool isBoolean(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* prim = type->as<PrimitiveTypeAST>()) {
        return prim->primitiveKind == PrimitiveKind::Bool;
    }
    return false;
}

// ============================================================================
// Callable Validation
// ============================================================================

bool isCallable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<FuncTypeAST>();
}

TypeAST* getReturnType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* func = type->as<FuncTypeAST>()) {
        if (!func->returnTypes.empty()) {
            return func->returnTypes[0];
        }
    }
    return nullptr;
}

std::vector<TypeAST*> getParameterTypes(TypeAST* type, TypeResolver& resolver) {
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

bool isArray(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<ArrayTypeAST>();
}

TypeAST* getElementType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* array = type->as<ArrayTypeAST>()) {
        return array->element;
    }
    return nullptr;
}

bool isSliceable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* array = type->as<ArrayTypeAST>()) {
        return true;  // All array kinds are sliceable
    }
    return false;
}

// ============================================================================
// Nullable Operations
// ============================================================================

bool isNullable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<NullableTypeAST>();
}

TypeAST* unwrapNullable(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* nullable = type->as<NullableTypeAST>()) {
        return nullable->inner;
    }
    return type;
}

bool isNilLiteral(ExprAST* expr) {
    if (auto* literal = expr->as<LiteralExprAST>()) {
        return literal->kind == LiteralKind::Nil;
    }
    return false;
}

// ============================================================================
// Result Type Operations
// ============================================================================

bool isResult(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    return type && type->isa<ResultTypeAST>();
}

TypeAST* getSuccessType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* result = type->as<ResultTypeAST>()) {
        return result->inner;
    }
    return nullptr;
}

TypeAST* getErrorType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    if (auto* result = type->as<ResultTypeAST>()) {
        return result->errorType;
    }
    return nullptr;
}

// ============================================================================
// Type Inference
// ============================================================================

TypeAST* unify(TypeAST* a, TypeAST* b, SemanticContext& ctx) {
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

TypeAST* commonType(TypeAST* a, TypeAST* b, SemanticContext& ctx) {
    // For binary operations, use unify
    return unify(a, b, ctx);
}

// ============================================================================
// Constant Evaluation
// ============================================================================

std::optional<int64_t> getConstantIntValue(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return std::nullopt;
    
    // Check if expression is marked as constant
    if (!expr->isConst) return std::nullopt;
    
    // Handle literal expressions
    if (auto* literal = expr->as<LiteralExprAST>()) {
        if (literal->kind == LiteralKind::Int) {
            // Parse integer value
            std::string_view val = ctx.pool.lookup(literal->value);
            return std::stoll(std::string(val));
        }
    }
    
    // TODO: Handle constant folding for binary operations, etc.
    
    return std::nullopt;
}

// ============================================================================
// Type Queries
// ============================================================================

TypeAST* getUnderlyingType(TypeAST* type, TypeResolver& resolver) {
    type = resolver.unwrapAlias(type);
    
    // Unwrap nullable
    if (isNullable(type, resolver)) {
        type = unwrapNullable(type, resolver);
    }
    
    return type;
}

bool isVoid(TypeAST* type) {
    return type == nullptr;
}

} // namespace TypeChecker