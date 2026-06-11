/**
 * @file ExprChecker.cpp
 * @brief Implementation of expression checkers.
 */

#include "ExprChecker.hpp"
#include "ast/DeclAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/resolver/TypeResolver.hpp"
#include "semantic/checker/TypeChecker.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace luc::checker {

// ============================================================================
// Dispatcher
// ============================================================================

TypeAST* checkExpr(ExprAST* expr, SemanticContext& ctx) {
    if (!expr) return nullptr;
    
    // Return cached result if already computed
    if (expr->resolvedType) {
        return expr->resolvedType;
    }
    
    LUC_LOG_SEMANTIC_EXTREME("checkExpr: kind=" << LucDebug::kindToString(expr->kind));
    
    TypeAST* result = nullptr;
    
    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = checkLiteralExpr(expr->as<LiteralExprAST>(), ctx);
            break;
        case ASTKind::ArrayLiteralExpr:
            result = checkArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx);
            break;
        case ASTKind::StructLiteralExpr:
            result = checkStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = checkIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
            break;
        case ASTKind::FieldAccessExpr:
            result = checkFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx);
            break;
        case ASTKind::BehaviorAccessExpr:
            result = checkBehaviorAccessExpr(expr->as<BehaviorAccessExprAST>(), ctx);
            break;
        case ASTKind::CallExpr:
            result = checkCallExpr(expr->as<CallExprAST>(), ctx);
            break;
        case ASTKind::IndexExpr:
            result = checkIndexExpr(expr->as<IndexExprAST>(), ctx);
            break;
        case ASTKind::SliceExpr:
            result = checkSliceExpr(expr->as<SliceExprAST>(), ctx);
            break;
        case ASTKind::UnaryExpr:
            result = checkUnaryExpr(expr->as<UnaryExprAST>(), ctx);
            break;
        case ASTKind::BinaryExpr:
            result = checkBinaryExpr(expr->as<BinaryExprAST>(), ctx);
            break;
        case ASTKind::AssignExpr:
            result = checkAssignExpr(expr->as<AssignExprAST>(), ctx);
            break;
        case ASTKind::IsExpr:
            result = checkIsExpr(expr->as<IsExprAST>(), ctx);
            break;
        case ASTKind::NullCoalesceExpr:
            result = checkNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx);
            break;
        case ASTKind::NullableChainExpr:
            result = checkNullableChainExpr(expr->as<NullableChainExprAST>(), ctx);
            break;
        case ASTKind::AnonFuncExpr:
            result = checkAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx);
            break;
        case ASTKind::AwaitExpr:
            result = checkAwaitExpr(expr->as<AwaitExprAST>(), ctx);
            break;
        case ASTKind::RangeExpr:
            result = checkRangeExpr(expr->as<RangeExprAST>(), ctx);
            break;
        default:
            LUC_LOG_SEMANTIC("checkExpr: unhandled expression kind: " 
                             << LucDebug::kindToString(expr->kind));
            break;
    }
    
    // Cache the result
    expr->resolvedType = result;
    return result;
}

// ============================================================================
// Literal Expressions
// ============================================================================

TypeAST* checkLiteralExpr(LiteralExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkLiteralExpr: kind=" << static_cast<int>(expr->kind));
    
    switch (expr->kind) {
        case LiteralKind::Int:
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int);
        case LiteralKind::Float:
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Float);
        case LiteralKind::String:
        case LiteralKind::RawString:
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::String);
        case LiteralKind::Char:
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Char);
        case LiteralKind::True:
        case LiteralKind::False:
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
        case LiteralKind::Nil:
            // nil is a special value; its type is determined by context
            // For now, return a nullable pointer type
            return nullptr;  // Will be resolved during assignment
        case LiteralKind::Hex:
        case LiteralKind::Binary:
            // These are integer literals with different syntax
            return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int);
        default:
            ctx.error(expr->loc, DiagCode::E2001, "unknown literal type");
            return nullptr;
    }
}

TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkArrayLiteralExpr: " << expr->elements.size() << " elements");
    
    if (expr->elements.empty()) {
        // Empty array literal – type must be inferred from context
        // For now, return unknown (will be resolved by surrounding context)
        return nullptr;
    }
    
    // Check all elements and find common type
    TypeAST* commonType = nullptr;
    
    for (auto* elem : expr->elements) {
        TypeAST* elemType = checkExpr(elem, ctx);
        if (!elemType) return nullptr;
        
        if (!commonType) {
            commonType = elemType;
        } else {
            commonType = TypeChecker::unify(commonType, elemType, ctx);
            if (!commonType) {
                ctx.error(elem->loc, DiagCode::E2001, 
                          "array elements have incompatible types");
                return nullptr;
            }
        }
    }
    
    // Create array type (slice by default)
    return ctx.arena.make<ArrayTypeAST>(ArrayKind::Slice, 0, commonType);
}

TypeAST* checkStructLiteralExpr(StructLiteralExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkStructLiteralExpr: " << ctx.pool.lookup(expr->typeName));
    
    // Look up the struct type
    TypeDeclAST* typeDecl = ctx.scope.lookupType(expr->typeName);
    if (!typeDecl) {
        ctx.error(expr->loc, DiagCode::E2001, 
                  "undefined struct type '", ctx.pool.lookup(expr->typeName), "'");
        return nullptr;
    }
    
    auto* structDecl = typeDecl->as<StructDeclAST>();
    if (!structDecl) {
        ctx.error(expr->loc, DiagCode::E2001, 
                  "'", ctx.pool.lookup(expr->typeName), "' is not a struct");
        return nullptr;
    }
    
    // Check field initializers
    for (auto* init : expr->inits) {
        if (!init) continue;
        
        // Check if field exists
        bool found = false;
        for (auto* field : structDecl->fields) {
            if (field->name == init->name) {
                found = true;
                
                // Check initializer expression
                TypeAST* initType = checkExpr(init->value, ctx);
                if (!initType) return nullptr;
                
                if (!TypeChecker::isAssignable(initType, field->valueType, ctx)) {
                    ctx.error(init->value->loc, DiagCode::E2001,
                              "cannot assign '", TypeChecker::getTypeName(initType, ctx.pool),
                              "' to field '", ctx.pool.lookup(field->name),
                              "' of type '", TypeChecker::getTypeName(field->valueType, ctx.pool), "'");
                    return nullptr;
                }
                break;
            }
        }
        
        if (!found) {
            ctx.error(init->loc, DiagCode::E2001,
                      "struct '", ctx.pool.lookup(expr->typeName), 
                      "' has no field named '", ctx.pool.lookup(init->name), "'");
            return nullptr;
        }
    }
    
    // Cache instantiated type
    expr->instantiatedType = structDecl->selfType;
    return structDecl->selfType;
}

// ============================================================================
// Identifier Expressions
// ============================================================================

TypeAST* checkIdentifierExpr(IdentifierExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIdentifierExpr: " << ctx.pool.lookup(expr->name));
    
    // First check value namespace
    ValueDeclAST* decl = ctx.scope.lookupValue(expr->name);
    if (decl) {
        // A value declaration (variable, function, parameter, etc.)
        if (auto* var = decl->as<VarDeclAST>()) {
            return var->valueType;
        }
        if (auto* func = decl->as<FuncDeclAST>()) {
            return func->funcType;
        }
        if (auto* param = decl->as<ParamAST>()) {
            return param->valueType;
        }
        if (auto* field = decl->as<FieldDeclAST>()) {
            return field->valueType;
        }
        if (auto* variant = decl->as<EnumVariantAST>()) {
            // Enum variant's type is the enum itself
            return ctx.scope.lookupType(variant->name)->selfType;
        }
        
        ctx.error(expr->loc, DiagCode::E2001,
                  "cannot determine type of '", ctx.pool.lookup(expr->name), "'");
        return nullptr;
    }
    
    // Then check type namespace (type used as value, e.g., int("42"))
    TypeDeclAST* typeDecl = ctx.scope.lookupType(expr->name);
    if (typeDecl) {
        // Type as a value (conversion function)
        return typeDecl->selfType;
    }
    
    ctx.error(expr->loc, DiagCode::E2001,
              "undefined identifier '", ctx.pool.lookup(expr->name), "'");
    return nullptr;
}

// ============================================================================
// Field and Method Access
// ============================================================================

TypeAST* checkFieldAccessExpr(FieldAccessExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkFieldAccessExpr: ." << ctx.pool.lookup(expr->field));
    
    // Check the object expression
    TypeAST* objType = checkExpr(expr->object, ctx);
    if (!objType) return nullptr;
    
    // Unwrap aliases and nullable
    objType = TypeChecker::getUnderlyingType(objType, *ctx.typeResolver);
    
    // Check if object is a struct
    if (auto* named = objType->as<NamedTypeAST>()) {
        TypeDeclAST* typeDecl = ctx.scope.lookupType(named->name);
        if (auto* structDecl = typeDecl->as<StructDeclAST>()) {
            // Look up field in struct's scope
            // Since fields are in struct's own scope, we need to search there
            // For now, iterate through fields
            for (auto* field : structDecl->fields) {
                if (field->name == expr->field) {
                    return field->valueType;
                }
            }
        }
    }
    
    ctx.error(expr->loc, DiagCode::E2001,
              "type has no field '", ctx.pool.lookup(expr->field), "'");
    return nullptr;
}

TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkBehaviorAccessExpr: :" << ctx.pool.lookup(expr->method));
    
    // Check the object expression
    TypeAST* objType = checkExpr(expr->object, ctx);
    if (!objType) return nullptr;
    
    // Unwrap aliases
    objType = TypeChecker::getUnderlyingType(objType, *ctx.typeResolver);
    
    // Look up method for this type
    // This requires the trait conformance map and method resolution
    // For now, return a placeholder function type
    // TODO: Implement method resolution
    
    ctx.error(expr->loc, DiagCode::E2001,
              "method '", ctx.pool.lookup(expr->method), 
              "' not found for type");
    return nullptr;
}

// ============================================================================
// Call Expressions
// ============================================================================

TypeAST* checkCallExpr(CallExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkCallExpr");
    
    // Check callee
    TypeAST* calleeType = checkExpr(expr->callee, ctx);
    if (!calleeType) return nullptr;
    
    // Check if callee is callable
    if (!TypeChecker::isCallable(calleeType, *ctx.typeResolver)) {
        ctx.error(expr->callee->loc, DiagCode::E2001,
                  "expression is not callable");
        return nullptr;
    }
    
    // Get parameter and return types
    std::vector<TypeAST*> paramTypes = TypeChecker::getParameterTypes(calleeType, *ctx.typeResolver);
    
    // Check argument count
    if (expr->args.size() != paramTypes.size()) {
        ctx.error(expr->loc, DiagCode::E2001,
                  "expected ", paramTypes.size(), " arguments, got ", expr->args.size());
        return nullptr;
    }
    
    // Check each argument
    for (size_t i = 0; i < expr->args.size(); ++i) {
        TypeAST* argType = checkExpr(expr->args[i], ctx);
        if (!argType) return nullptr;
        
        if (!TypeChecker::isAssignable(argType, paramTypes[i], ctx)) {
            ctx.error(expr->args[i]->loc, DiagCode::E2001,
                      "argument ", i + 1, " has wrong type");
            return nullptr;
        }
    }
    
    return TypeChecker::getReturnType(calleeType, *ctx.typeResolver);
}

// ============================================================================
// Index and Slice Expressions
// ============================================================================

TypeAST* checkIndexExpr(IndexExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIndexExpr");
    
    // Check target
    TypeAST* targetType = checkExpr(expr->target, ctx);
    if (!targetType) return nullptr;
    
    targetType = TypeChecker::getUnderlyingType(targetType, *ctx.typeResolver);
    
    // Check if target is an array
    if (!TypeChecker::isArray(targetType, *ctx.typeResolver)) {
        ctx.error(expr->target->loc, DiagCode::E2001,
                  "cannot index non-array type");
        return nullptr;
    }
    
    // Check index type (must be integer)
    TypeAST* indexType = checkExpr(expr->index, ctx);
    if (!indexType) return nullptr;
    
    if (!TypeChecker::isInteger(indexType, *ctx.typeResolver)) {
        ctx.error(expr->index->loc, DiagCode::E2001,
                  "array index must be integer");
        return nullptr;
    }
    
    return TypeChecker::getElementType(targetType, *ctx.typeResolver);
}

TypeAST* checkSliceExpr(SliceExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkSliceExpr");
    
    // Check target
    TypeAST* targetType = checkExpr(expr->target, ctx);
    if (!targetType) return nullptr;
    
    targetType = TypeChecker::getUnderlyingType(targetType, *ctx.typeResolver);
    
    // Check if target is sliceable
    if (!TypeChecker::isSliceable(targetType, *ctx.typeResolver)) {
        ctx.error(expr->target->loc, DiagCode::E2001,
                  "cannot slice non-array type");
        return nullptr;
    }
    
    // Check start type (if present)
    if (expr->start) {
        TypeAST* startType = checkExpr(expr->start, ctx);
        if (!startType) return nullptr;
        
        if (!TypeChecker::isInteger(startType, *ctx.typeResolver)) {
            ctx.error(expr->start->loc, DiagCode::E2001,
                      "slice start must be integer");
            return nullptr;
        }
    }
    
    // Check end type (if present)
    if (expr->end) {
        TypeAST* endType = checkExpr(expr->end, ctx);
        if (!endType) return nullptr;
        
        if (!TypeChecker::isInteger(endType, *ctx.typeResolver)) {
            ctx.error(expr->end->loc, DiagCode::E2001,
                      "slice end must be integer");
            return nullptr;
        }
    }
    
    // Slice always returns a slice type
    TypeAST* elemType = TypeChecker::getElementType(targetType, *ctx.typeResolver);
    TypeAST* sliceType = ctx.arena.make<ArrayTypeAST>(ArrayKind::Slice, 0, elemType);
    
    // Cache the slice type
    expr->sliceType = sliceType;
    return sliceType;
}

// ============================================================================
// Unary Expressions
// ============================================================================

TypeAST* checkUnaryExpr(UnaryExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkUnaryExpr: op=" << static_cast<int>(expr->op));
    
    TypeAST* operandType = checkExpr(expr->operand, ctx);
    if (!operandType) return nullptr;
    
    operandType = TypeChecker::getUnderlyingType(operandType, *ctx.typeResolver);
    
    switch (expr->op) {
        case UnaryOp::Neg:
            if (!TypeChecker::isNumeric(operandType, *ctx.typeResolver)) {
                ctx.error(expr->operand->loc, DiagCode::E2001,
                          "negation requires numeric operand");
                return nullptr;
            }
            return operandType;
            
        case UnaryOp::Not:
            if (!TypeChecker::isBoolean(operandType, *ctx.typeResolver)) {
                ctx.error(expr->operand->loc, DiagCode::E2001,
                          "logical not requires boolean operand");
                return nullptr;
            }
            return operandType;
            
        case UnaryOp::BitNot:
            if (!TypeChecker::isInteger(operandType, *ctx.typeResolver)) {
                ctx.error(expr->operand->loc, DiagCode::E2001,
                          "bitwise not requires integer operand");
                return nullptr;
            }
            return operandType;
            
        case UnaryOp::Ref:
            // Reference operator &T
            return ctx.arena.make<RefTypeAST>(operandType);
            
        default:
            ctx.error(expr->loc, DiagCode::E2001, "unknown unary operator");
            return nullptr;
    }
}

// ============================================================================
// Binary Expressions
// ============================================================================

TypeAST* checkBinaryExpr(BinaryExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkBinaryExpr: op=" << static_cast<int>(expr->op));
    
    TypeAST* leftType = checkExpr(expr->left, ctx);
    TypeAST* rightType = checkExpr(expr->right, ctx);
    if (!leftType || !rightType) return nullptr;
    
    leftType = TypeChecker::getUnderlyingType(leftType, *ctx.typeResolver);
    rightType = TypeChecker::getUnderlyingType(rightType, *ctx.typeResolver);
    
    // Determine operation category
    bool isComparison = false;
    bool isLogical = false;
    bool isBitwise = false;
    
    switch (expr->op) {
        // Comparison operators
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge:
        case BinaryOp::RefEq:
            isComparison = true;
            break;
            
        // Logical operators
        case BinaryOp::And:
        case BinaryOp::Or:
            isLogical = true;
            break;
            
        // Bitwise operators
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr:
            isBitwise = true;
            break;
            
        // Arithmetic operators
        default:
            break;
    }
    
    if (isComparison) {
        // Comparison requires comparable types
        if (!TypeChecker::typesEqualForComparison(leftType, rightType, *ctx.typeResolver)) {
            ctx.error(expr->loc, DiagCode::E2001,
                      "cannot compare different types");
            return nullptr;
        }
        return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
    }
    
    if (isLogical) {
        // Logical requires boolean types
        if (!TypeChecker::isBoolean(leftType, *ctx.typeResolver) ||
            !TypeChecker::isBoolean(rightType, *ctx.typeResolver)) {
            ctx.error(expr->loc, DiagCode::E2001,
                      "logical operators require boolean operands");
            return nullptr;
        }
        return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
    }
    
    if (isBitwise) {
        // Bitwise requires integer types
        if (!TypeChecker::isInteger(leftType, *ctx.typeResolver) ||
            !TypeChecker::isInteger(rightType, *ctx.typeResolver)) {
            ctx.error(expr->loc, DiagCode::E2001,
                      "bitwise operators require integer operands");
            return nullptr;
        }
        // Result type is common type
        return TypeChecker::commonType(leftType, rightType, ctx);
    }
    
    // Arithmetic operators (+, -, *, /, ^, %)
    if (!TypeChecker::isNumeric(leftType, *ctx.typeResolver) ||
        !TypeChecker::isNumeric(rightType, *ctx.typeResolver)) {
        ctx.error(expr->loc, DiagCode::E2001,
                  "arithmetic operators require numeric operands");
        return nullptr;
    }
    
    return TypeChecker::commonType(leftType, rightType, ctx);
}

// ============================================================================
// Assignment Expressions
// ============================================================================

TypeAST* checkAssignExpr(AssignExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkAssignExpr");
    
    // Check RHS first (it may determine LHS type for inference)
    TypeAST* rhsType = checkExpr(expr->rhs, ctx);
    if (!rhsType) return nullptr;
    
    // Check LHS (must be assignable)
    TypeAST* lhsType = checkExpr(expr->lhs, ctx);
    if (!lhsType) return nullptr;
    
    if (!TypeChecker::isAssignable(rhsType, lhsType, ctx)) {
        ctx.error(expr->loc, DiagCode::E2001, "cannot assign value to left-hand side");
        return nullptr;
    }
    
    // For compound assignment (e.g., +=), also need to check the operation
    if (expr->op != AssignOp::Assign) {
        // Compound assignment requires numeric types
        if (!TypeChecker::isNumeric(lhsType, *ctx.typeResolver)) {
            ctx.error(expr->loc, DiagCode::E2001, "compound assignment requires numeric type");
            return nullptr;
        }
    }
    
    return lhsType;
}

// ============================================================================
// Other Expressions
// ============================================================================

TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkNullCoalesceExpr");
    
    TypeAST* valueType = checkExpr(expr->value, ctx);
    TypeAST* fallbackType = checkExpr(expr->fallback, ctx);
    if (!valueType || !fallbackType) return nullptr;
    
    // Value must be nullable
    if (!TypeChecker::isNullable(valueType, *ctx.typeResolver)) {
        ctx.error(expr->value->loc, DiagCode::E2001,
                  "left-hand side of '??' must be nullable");
        return nullptr;
    }
    
    // Fallback must be assignable to the unwrapped type
    TypeAST* unwrapped = TypeChecker::unwrapNullable(valueType, *ctx.typeResolver);
    if (!TypeChecker::isAssignable(fallbackType, unwrapped, ctx)) {
        ctx.error(expr->fallback->loc, DiagCode::E2001,
                  "fallback value type does not match");
        return nullptr;
    }
    
    return unwrapped;
}

TypeAST* checkNullableChainExpr(NullableChainExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkNullableChainExpr: " << expr->steps.size() << " steps");
    
    TypeAST* currentType = checkExpr(expr->object, ctx);
    if (!currentType) return nullptr;
    
    for (const auto& step : expr->steps) {
        // Current type must be nullable
        if (!TypeChecker::isNullable(currentType, *ctx.typeResolver)) {
            ctx.error(expr->object->loc, DiagCode::E2001,
                      "cannot chain '?.' on non-nullable type");
            return nullptr;
        }
        
        // Unwrap to get the underlying type
        currentType = TypeChecker::unwrapNullable(currentType, *ctx.typeResolver);
        
        // Must be a struct with the field
        // TODO: Look up field in struct
        // For now, return unknown
    }
    
    // Result is nullable
    return ctx.arena.make<NullableTypeAST>(currentType);
}

TypeAST* checkIsExpr(IsExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIsExpr");
    
    TypeAST* exprType = checkExpr(expr->expr, ctx);
    if (!exprType) return nullptr;
    
    // Resolve the check type
    TypeAST* checkType = ctx.typeResolver->resolve(expr->checkType);
    if (!checkType) return nullptr;
    
    // Can always check if expression is a type
    // The type checker will narrow the type in the appropriate context
    return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
}

TypeAST* checkAnonFuncExpr(AnonFuncExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkAnonFuncExpr");
    
    // Resolve the function type
    if (expr->funcType) {
        TypeAST* resolved = ctx.typeResolver->resolve(expr->funcType);
        if (!resolved) return nullptr;
        
        expr->funcType = resolved->as<FuncTypeAST>();
        return expr->funcType;
    }
    
    ctx.error(expr->loc, DiagCode::E2001, "anonymous function has no type");
    return nullptr;
}

TypeAST* checkAwaitExpr(AwaitExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkAwaitExpr");
    
    // Check we're in an async context
    // For now, just check the inner expression
    TypeAST* innerType = checkExpr(expr->inner, ctx);
    if (!innerType) return nullptr;
    
    // TODO: Check that innerType is a future type
    // For now, return inner type
    
    return innerType;
}

TypeAST* checkRangeExpr(RangeExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkRangeExpr");
    
    TypeAST* loType = checkExpr(expr->lo, ctx);
    TypeAST* hiType = checkExpr(expr->hi, ctx);
    if (!loType || !hiType) return nullptr;
    
    loType = TypeChecker::getUnderlyingType(loType, *ctx.typeResolver);
    hiType = TypeChecker::getUnderlyingType(hiType, *ctx.typeResolver);
    
    if (!TypeChecker::isInteger(loType, *ctx.typeResolver) ||
        !TypeChecker::isInteger(hiType, *ctx.typeResolver)) {
        ctx.error(expr->loc, DiagCode::E2001, "range bounds must be integers");
        return nullptr;
    }
    
    // Range type - for now, just return int
    return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Int);
}

} // namespace luc::checker