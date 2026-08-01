/// @file SemaExpr.cpp
/// @brief Implements Sema.hpp's "EXPRESSIONS - Type Resolution" section.
/// 
/// @design_decision Direct Expression Mutation
///   Each resolver updates the ExprAST node directly (resolvedType, valueState).
///   This leverages the existing infrastructure and avoids duplication.
/// 
/// @design_decision Target Type Validation
///   `resolveExprWithTarget` validates expressions against an expected type.
///   This centralizes type checking and uses cached singleton types.

#include "../Sema.hpp"

#include <unordered_set>
#include <optional>

namespace sema {

// ─── Helper: Set expression result ──────────────────────────────────────

/// @brief Set an expression's resolved type and value state.
static void setExprResult(ExprAST* expr, TypeAST* type, ValueState state) {
    expr->resolvedType = type;
    expr->valueState = state;
}

/// @brief Set an expression to error state.
static void setExprError(ExprAST* expr, SemaContext& ctx, const BaseAST* node,
                         DiagCode code, const std::string& msg) {
    ctx.error(node, code, msg);
    expr->resolvedType = ctx.getUnknownType();
    expr->valueState = ValueState::Unknown;
}

// =============================================================================
// resolveExprWithTarget - Main Entry Point
// =============================================================================

TypeAST* resolveExprWithTarget(ExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr) {
        return ctx.getUnknownType();
    }

    TypeAST* result = nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = resolveLiteralExpr(expr->as<LiteralExprAST>(), targetType, ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = resolveIdentifierExpr(expr->as<IdentifierExprAST>(), targetType, ctx);
            break;
        case ASTKind::ArrayLiteralExpr:
            result = resolveArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), targetType, ctx);
            break;
        case ASTKind::StructLiteralExpr:
            result = resolveStructLiteralExpr(expr->as<StructLiteralExprAST>(), targetType, ctx);
            break;
        case ASTKind::BinaryExpr:
            result = resolveBinaryExpr(expr->as<BinaryExprAST>(), targetType, ctx);
            break;
        case ASTKind::UnaryExpr:
            result = resolveUnaryExpr(expr->as<UnaryExprAST>(), targetType, ctx);
            break;
        case ASTKind::CallExpr:
            result = resolveCallExpr(expr->as<CallExprAST>(), targetType, ctx);
            break;
        case ASTKind::IntrinsicCallExpr:
            result = resolveIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), targetType, ctx);
            break;
        case ASTKind::IndexExpr:
            result = resolveIndexExpr(expr->as<IndexExprAST>(), targetType, ctx);
            break;
        case ASTKind::SliceExpr:
            result = resolveSliceExpr(expr->as<SliceExprAST>(), targetType, ctx);
            break;
        case ASTKind::FieldAccessExpr:
            result = resolveFieldAccessExpr(expr->as<FieldAccessExprAST>(), targetType, ctx);
            break;
        case ASTKind::ModuleAccessExpr:
            result = resolveModuleAccessExpr(expr->as<ModuleAccessExprAST>(), targetType, ctx);
            break;
        case ASTKind::NullableChainExpr:
            result = resolveNullableChainExpr(expr->as<NullableChainExprAST>(), targetType, ctx);
            break;
        case ASTKind::NullCoalesceExpr:
            result = resolveNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), targetType, ctx);
            break;
        case ASTKind::AssignExpr:
            result = resolveAssignExpr(expr->as<AssignExprAST>(), targetType, ctx);
            break;
        case ASTKind::PipelineExpr:
            result = resolvePipelineExpr(expr->as<PipelineExprAST>(), targetType, ctx);
            break;
        case ASTKind::ComposeExpr:
            result = resolveComposeExpr(expr->as<ComposeExprAST>(), targetType, ctx);
            break;
        case ASTKind::AnonFuncExpr:
            result = resolveAnonFuncExpr(expr->as<AnonFuncExprAST>(), targetType, ctx);
            break;
        case ASTKind::IfExpr:
            result = resolveIfExpr(expr->as<IfExprAST>(), targetType, ctx);
            break;
        case ASTKind::RangeExpr:
            result = resolveRangeExpr(expr->as<RangeExprAST>(), targetType, ctx);
            break;
        default:
            setExprError(expr, ctx, expr, DiagCode::E3003, "unsupported expression kind");
            return ctx.getUnknownType();
    }

    // Store the result on the expression (if not already stored by resolver)
    if (result && !expr->resolvedType) {
        expr->resolvedType = result;
    }
    if (!result || result->isa<UnknownTypeAST>()) {
        expr->valueState = ValueState::Unknown;
        return result;
    }

    // Validate against target type if provided and if we have a valid type
    if (targetType && result && !result->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, result, ctx)) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "type mismatch: expected " +
                         debug::typeToString(targetType, ctx.pool) +
                         ", got " +
                         debug::typeToString(result, ctx.pool));
            return ctx.getUnknownType();
        }
    }

    return result;
}

/// @brief Legacy entry point for backward compatibility.
TypeAST* resolveExpr(ExprAST* expr, SemaContext& ctx) {
    return resolveExprWithTarget(expr, nullptr, ctx);
}

// =============================================================================
// resolveLiteralExpr
// =============================================================================

TypeAST* resolveLiteralExpr(LiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    TypeAST* result = nullptr;
    ValueState state = ValueState::Definite;

    switch (expr->kind) {
        case LiteralKind::True:
        case LiteralKind::False:
            result = ctx.getBoolType();
            state = ValueState::Definite;
            break;

        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
            if (targetType && targetType->isa<PrimitiveTypeAST>() && isIntegerType(targetType)) {
                result = const_cast<TypeAST*>(targetType);
            } else {
                result = ctx.getIntType();
            }
            state = ValueState::Definite;
            break;

        case LiteralKind::Float:
            if (targetType && targetType->isa<PrimitiveTypeAST>() && isFloatType(targetType)) {
                result = const_cast<TypeAST*>(targetType);
            } else {
                result = ctx.getFloatType();
            }
            state = ValueState::Definite;
            break;

        case LiteralKind::String:
        case LiteralKind::RawString:
            result = ctx.getStringType();
            state = ValueState::Definite;
            break;

        case LiteralKind::Char:
            result = ctx.getCharType();
            state = ValueState::Definite;
            break;

        case LiteralKind::Nil:
            if (targetType && isNullableType(targetType)) {
                result = const_cast<TypeAST*>(targetType);
                state = ValueState::Nil;
            } else {
                result = ctx.getUnknownType();
                state = ValueState::Nil;
            }
            break;

        case LiteralKind::Err:
            if (targetType && isFallibleType(targetType)) {
                result = const_cast<TypeAST*>(targetType);
                state = ValueState::Err;
            } else {
                result = ctx.getUnknownType();
                state = ValueState::Err;
            }
            break;

        default:
            setExprError(expr, ctx, expr, DiagCode::E3003, "unknown literal kind");
            return ctx.getUnknownType();
    }

    setExprResult(expr, result, state);
    return result;
}

// =============================================================================
// resolveIdentifierExpr
// =============================================================================

TypeAST* resolveIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Check if this is a generic parameter ─────────────────────
    if (ctx.isGenericParam(expr->name)) {
        setExprError(expr, ctx, expr, DiagCode::E2003,
                     "'" + ctx.pool.lookup(expr->name) + "' is a generic type parameter, not a value");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Look up the value declaration ────────────────────────────
    const ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) {
        setExprError(expr, ctx, expr, DiagCode::E2001,
                     "undefined value '" + ctx.pool.lookup(expr->name) + "'");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Determine value state ─────────────────────────────────────
    ValueState state = ValueState::Unknown;
    if (decl->isa<EnumVariantAST>() || decl->isa<FuncDeclAST>()) {
        state = ValueState::Definite;
    } else if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* var = decl->as<VarDeclAST>();
        if (var->init && var->init->isConst) {
            state = ValueState::Definite;
        } else {
            state = ValueState::Unknown;
        }
    } else {
        state = ValueState::Unknown;
    }

    // ─── Step 4: Handle generic arguments (function instantiation) ────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "'" + ctx.pool.lookup(expr->name) + "' is not a function");
            return ctx.getUnknownType();
        }

        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                setExprError(expr, ctx, expr, DiagCode::E3002,
                             "invalid generic argument type for '" +
                             ctx.pool.lookup(expr->name) + "'");
                return ctx.getUnknownType();
            }
        }

        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            return ctx.getUnknownType();
        }
    }

    // ─── Step 5: Return the declaration's type ─────────────────────────────
    if (!decl->type) {
        setExprError(expr, ctx, expr, DiagCode::E2002,
                     "'" + ctx.pool.lookup(expr->name) + "' has no type information");
        return ctx.getUnknownType();
    }

    setExprResult(expr, decl->type, state);
    return decl->type;
}

// =============================================================================
// resolveArrayLiteralExpr
// =============================================================================

TypeAST* resolveArrayLiteralExpr(ArrayLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (expr->elements.empty()) {
        setExprResult(expr, ctx.getUnknownType(), ValueState::Definite);
        return ctx.getUnknownType();
    }

    // Resolve the first element (with target element type if available)
    const TypeAST* targetElemType = nullptr;
    if (targetType && targetType->isa<ArrayTypeAST>()) {
        targetElemType = targetType->as<ArrayTypeAST>()->element;
    }

    TypeAST* firstType = resolveExprWithTarget(expr->elements[0], targetElemType, ctx);
    if (!firstType || firstType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr, DiagCode::E3003, "array literal element has unknown type");
        return ctx.getUnknownType();
    }

    // Check all elements against the first type
    bool allMatch = true;
    bool hasErr = false;
    bool allDefinite = true;

    for (size_t i = 1; i < expr->elements.size(); ++i) {
        TypeAST* elemType = resolveExprWithTarget(expr->elements[i], firstType, ctx);
        if (!elemType || elemType->isa<UnknownTypeAST>()) {
            allMatch = false;
            continue;
        }

        if (!typesEqual(firstType, elemType)) {
            allMatch = false;
            break;
        }

        if (expr->elements[i]->valueState == ValueState::Err) {
            hasErr = true;
        }
        if (expr->elements[i]->valueState != ValueState::Definite) {
            allDefinite = false;
        }
    }

    if (!allMatch) {
        ctx.error(expr, DiagCode::E3003,
                  "array literal contains elements of different types");
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // Propagate value state
    ValueState state;
    if (hasErr) {
        state = ValueState::Err;
    } else if (allDefinite) {
        state = ValueState::Definite;
    } else {
        state = ValueState::Unknown;
    }

    // Use cached array type
    ArrayTypeAST* arrayType = ctx.getArrayType(ArrayKind::Dynamic, 0, firstType);
    setExprResult(expr, arrayType, state);
    return arrayType;
}

// =============================================================================
// resolveStructLiteralExpr
// =============================================================================

TypeAST* resolveStructLiteralExpr(StructLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Look up the struct type ─────────────────────────────────
    const TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        ctx.error(expr, DiagCode::E2002,
                  "undefined type '", ctx.pool.lookup(expr->typeName), "'");
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.error(expr, DiagCode::E2002,
                  "'", ctx.pool.lookup(expr->typeName), "' is not a struct");
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // ─── Step 2: Check and validate generic arguments ────────────────────
    if (!expr->genericArgs.empty()) {
        // ─── 2a. Check arity ─────────────────────────────────────────────
        if (expr->genericArgs.size() != structDecl->genericParams.size()) {
            ctx.error(expr, DiagCode::E2207,
                      "struct '", ctx.pool.lookup(structDecl->name),
                      "' expected ", std::to_string(structDecl->genericParams.size()),
                      " generic arguments, got ",
                      std::to_string(expr->genericArgs.size()));
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }

        // ─── 2b. Resolve each generic argument type ──────────────────────
        // Create a temporary array to store resolved arguments
        // We need to resolve each argument and store it back
        for (size_t i = 0; i < expr->genericArgs.size(); ++i) {
            TypeAST* resolvedArg = resolveType(expr->genericArgs[i], ctx);
            if (!resolvedArg) {
                ctx.error(expr, DiagCode::E3002,
                          "invalid generic argument at position ", std::to_string(i + 1),
                          " for struct '", ctx.pool.lookup(structDecl->name), "'");
                setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
                return ctx.getUnknownType();
            }
            // Update the generic argument with the resolved type
            const_cast<TypePtr&>(expr->genericArgs[i]) = resolvedArg;
        }

        // ─── 2c. Validate constraints ─────────────────────────────────────
        // Use the existing validateGenericArguments function
        // Note: The generic arguments in StructLiteralExprAST are stored as TypePtr,
        // but validateGenericArguments expects ArenaSpan<TypePtr>.
        // We need to create a temporary ArenaSpan or use the existing one.
        if (!validateGenericArguments(expr->genericArgs, structDecl->genericParams, expr, ctx)) {
            // Error already reported by validateGenericArguments
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }
    } else if (!structDecl->genericParams.empty()) {
        // ─── 2d. Struct has generic parameters but no arguments provided ──
        ctx.error(expr, DiagCode::E2207,
                  "struct '", ctx.pool.lookup(structDecl->name),
                  "' requires ", std::to_string(structDecl->genericParams.size()),
                  " generic argument(s)");
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // ─── Step 3: Build field map ─────────────────────────────────────────
    std::unordered_map<InternedString, const FieldDeclAST*> fieldMap;
    for (const FieldDeclAST* field : structDecl->fields) {
        fieldMap[field->name] = field;
    }

    std::unordered_set<InternedString> initializedFields;
    bool hasErr = false;
    bool allDefinite = true;

    // ─── Step 4: Validate each field initializer ─────────────────────────
    for (const FieldInitAST* init : expr->inits) {
        auto it = fieldMap.find(init->name);
        if (it == fieldMap.end()) {
            ctx.error(init, DiagCode::E2001,
                      "struct '", ctx.pool.lookup(structDecl->name),
                      "' has no field named '", ctx.pool.lookup(init->name), "'");
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }

        const FieldDeclAST* field = it->second;

        // ─── 4a. Const field validation ─────────────────────────────────
        if (field->isConst) {
            if (init->value->isa<LiteralExprAST>()) {
                const LiteralExprAST* literal = init->value->as<LiteralExprAST>();
                if (literal->kind == LiteralKind::Nil || literal->kind == LiteralKind::Err) {
                    ctx.error(init, DiagCode::E3004,
                              "const field '", ctx.pool.lookup(field->name),
                              "' cannot be assigned '",
                              (literal->kind == LiteralKind::Nil ? "nil" : "err"),
                              "' (const fields must have definite values)");
                    setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
                    return ctx.getUnknownType();
                }
            }
        }

        // ─── 4b. Resolve initializer against the field type ─────────────
        TypeAST* initType = resolveExprWithTarget(init->value, field->type, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }

        if (init->value->valueState == ValueState::Err) {
            hasErr = true;
        }
        if (init->value->valueState != ValueState::Definite) {
            allDefinite = false;
        }

        initializedFields.insert(init->name);
    }

    // ─── Step 5: Check for missing required fields ──────────────────────
    for (const FieldDeclAST* field : structDecl->fields) {
        if (initializedFields.find(field->name) != initializedFields.end()) {
            continue;
        }

        if (field->defaultVal) {
            // Has default value - OK
            continue;
        }

        if (isNullableType(field->type)) {
            // Nullable fields can be omitted (defaults to nil)
            continue;
        }

        if (isFallibleType(field->type)) {
            // Fallible fields can be omitted (defaults to err)
            continue;
        }

        if (field->type->isa<CombinedTypeAST>()) {
            ctx.error(expr, DiagCode::E3002,
                      "combined field '", ctx.pool.lookup(field->name),
                      "' (T?!) must be explicitly initialized (no implicit default)");
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }

        ctx.error(expr, DiagCode::E3002,
                  "field '", ctx.pool.lookup(field->name),
                  "' must be initialized in struct literal (no default value)");
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // ─── Step 6: Propagate value state ──────────────────────────────────
    ValueState state;
    if (hasErr) {
        state = ValueState::Err;
    } else if (allDefinite && !expr->inits.empty()) {
        state = ValueState::Definite;
    } else {
        state = ValueState::Unknown;
    }

    // ─── Step 7: Return the struct type (cached) ──────────────────────
    NamedTypeAST* resultType = ctx.getNamedType(structDecl->name);
    setExprResult(expr, resultType, state);
    return resultType;
}

// =============================================================================
// resolveBinaryExpr
// =============================================================================

TypeAST* resolveBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve operands ──────────────────────────────────────────
    TypeAST* leftType = resolveExpr(expr->left, ctx);
    if (!leftType || leftType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->left, DiagCode::E3003, "left operand has unknown type");
        return ctx.getUnknownType();
    }

    TypeAST* rightType = resolveExpr(expr->right, ctx);
    if (!rightType || rightType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->right, DiagCode::E3003, "right operand has unknown type");
        return ctx.getUnknownType();
    }

    ValueState leftState = expr->left->valueState;
    ValueState rightState = expr->right->valueState;

    // ─── Step 2: Check if we're in an if condition context ────────────────
    if (ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.stack.setPendingNarrowing(info);
            setExprResult(expr, ctx.getBoolType(), ValueState::Definite);
            return ctx.getBoolType();
        }
    }

    // ─── Step 3: Validate operator-specific rules ──────────────────────────
    TypeAST* resultType = nullptr;
    ValueState resultState = ValueState::Definite;

    switch (expr->op) {
        // ─── Arithmetic Operators ──────────────────────────────────────────
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::Mod: {
            if (leftState == ValueState::Nil || rightState == ValueState::Nil) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "arithmetic operator cannot be used with nil. Use `??` to handle nil first.");
                return ctx.getUnknownType();
            }

            if (!isNumericType(leftType) || !isNumericType(rightType)) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "arithmetic operator requires numeric operands");
                return ctx.getUnknownType();
            }

            resultType = isFloatType(rightType) ? rightType : leftType;
            resultState = (leftState == ValueState::Err || rightState == ValueState::Err)
                          ? ValueState::Err : ValueState::Definite;
            break;
        }

        // ─── Comparison Operators ──────────────────────────────────────────
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge: {
            resultType = ctx.getBoolType();
            resultState = ValueState::Definite;
            break;
        }

        // ─── Logical Operators ─────────────────────────────────────────────
        case BinaryOp::And:
        case BinaryOp::Or: {
            if (leftState == ValueState::Nil || rightState == ValueState::Nil) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "logical operator cannot be used with nil");
                return ctx.getUnknownType();
            }

            if (leftState == ValueState::Err || rightState == ValueState::Err) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "logical operator cannot be used with err");
                return ctx.getUnknownType();
            }

            if (!isBoolType(leftType) || !isBoolType(rightType)) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "logical operator requires bool operands");
                return ctx.getUnknownType();
            }

            resultType = ctx.getBoolType();
            resultState = ValueState::Definite;
            break;
        }

        // ─── Bitwise Operators ─────────────────────────────────────────────
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            if (leftState == ValueState::Nil || rightState == ValueState::Nil) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "bitwise operator cannot be used with nil");
                return ctx.getUnknownType();
            }

            if (leftState == ValueState::Err || rightState == ValueState::Err) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "bitwise operator cannot be used with err");
                return ctx.getUnknownType();
            }

            if (!isIntegerType(leftType) || !isIntegerType(rightType)) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "bitwise operator requires integer operands");
                return ctx.getUnknownType();
            }

            resultType = leftType;
            resultState = ValueState::Definite;
            break;
        }

        default:
            setExprError(expr, ctx, expr, DiagCode::E3003, "unknown binary operator");
            return ctx.getUnknownType();
    }

    // Validate against target type if provided
    if (targetType && resultType && !resultType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, resultType, ctx)) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "type mismatch: expected " +
                         debug::typeToString(targetType, ctx.pool) +
                         ", got " +
                         debug::typeToString(resultType, ctx.pool));
            return ctx.getUnknownType();
        }
    }

    setExprResult(expr, resultType, resultState);
    return resultType;
}

// =============================================================================
// resolveUnaryExpr
// =============================================================================

TypeAST* resolveUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    TypeAST* operandType = resolveExpr(expr->operand, ctx);
    if (!operandType || operandType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->operand, DiagCode::E3003, "operand has unknown type");
        return ctx.getUnknownType();
    }

    ValueState operandState = expr->operand->valueState;
    bool isNil = operandState == ValueState::Nil;
    bool isErr = operandState == ValueState::Err;

    TypeAST* resultType = nullptr;
    ValueState resultState = ValueState::Definite;

    switch (expr->op) {
        case UnaryOp::Neg: {
            if (isNil) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "negation cannot be used with nil. Use `??` to handle nil first.");
                return ctx.getUnknownType();
            }

            if (isErr) {
                resultType = operandType;
                resultState = ValueState::Err;
                break;
            }

            if (!isNumericType(operandType)) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "negation requires numeric operand");
                return ctx.getUnknownType();
            }

            resultType = operandType;
            resultState = ValueState::Definite;
            break;
        }

        case UnaryOp::Not: {
            if (isNil) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "logical not cannot be used with nil. Use `??` to handle nil first.");
                return ctx.getUnknownType();
            }

            if (isErr) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "logical not cannot be used with err. Use `??` to handle err first.");
                return ctx.getUnknownType();
            }

            if (!isBoolType(operandType)) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "logical not requires bool operand");
                return ctx.getUnknownType();
            }

            resultType = ctx.getBoolType();
            resultState = ValueState::Definite;
            break;
        }

        case UnaryOp::BitNot: {
            if (isNil) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "bitwise not cannot be used with nil. Use `??` to handle nil first.");
                return ctx.getUnknownType();
            }

            if (isErr) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "bitwise not cannot be used with err. Use `??` to handle err first.");
                return ctx.getUnknownType();
            }

            if (!isIntegerType(operandType)) {
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "bitwise not requires integer operand");
                return ctx.getUnknownType();
            }

            resultType = operandType;
            resultState = ValueState::Definite;
            break;
        }

        default:
            setExprError(expr, ctx, expr, DiagCode::E3003, "unknown unary operator");
            return ctx.getUnknownType();
    }

    setExprResult(expr, resultType, resultState);
    return resultType;
}

// =============================================================================
// resolveCallExpr
// =============================================================================

TypeAST* resolveCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve callee ─────────────────────────────────────────────
    TypeAST* calleeType = resolveExpr(expr->callee, ctx);
    if (!calleeType || calleeType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->callee, DiagCode::E2003, "callee has unknown type");
        return ctx.getUnknownType();
    }

    if (expr->callee->valueState == ValueState::Nil) {
        setExprError(expr, ctx, expr->callee, DiagCode::E3003,
                     "cannot call nil value. Use `??` to handle nil first.");
        return ctx.getUnknownType();
    }

    if (expr->callee->valueState == ValueState::Err) {
        setExprError(expr, ctx, expr->callee, DiagCode::E3003,
                     "cannot call err value. Use `??` to handle err first.");
        return ctx.getUnknownType();
    }

    if (!calleeType->isa<FuncTypeAST>()) {
        setExprError(expr, ctx, expr->callee, DiagCode::E2003,
                     "expression is not callable");
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = calleeType->as<FuncTypeAST>();

    // ─── Step 2: Check generic arguments ────────────────────────────────────
    const FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);
    if (funcDecl) {
        if (!expr->genericArgs.empty()) {
            for (const TypeAST* arg : expr->genericArgs) {
                if (!resolveType(arg, ctx)) {
                    setExprError(expr, ctx, expr, DiagCode::E3002,
                                 "invalid generic argument type for '" +
                                 ctx.pool.lookup(funcDecl->name) + "'");
                    return ctx.getUnknownType();
                }
            }

            if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
                return ctx.getUnknownType();
            }
        } else if (!funcDecl->genericParams.empty()) {
            ctx.error(expr, DiagCode::E2207,
                      "generic function '", ctx.pool.lookup(funcDecl->name),
                      "' requires ", std::to_string(funcDecl->genericParams.size()),
                      " generic argument(s)");
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }
    } else {
        if (!expr->genericArgs.empty()) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "generic arguments can only be applied to named function calls");
            return ctx.getUnknownType();
        }
    }

    // ─── Step 3: Check argument count ──────────────────────────────────────
    if (expr->args.size() != funcType->params.size()) {
        ctx.error(expr, DiagCode::E3001,
                  "wrong number of arguments: expected ",
                  std::to_string(funcType->params.size()), ", found ",
                  std::to_string(expr->args.size()));
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // ─── Step 4: Check each argument type ──────────────────────────────────
    bool hasErrArg = false;
    for (size_t i = 0; i < expr->args.size(); ++i) {
        ExprAST* arg = expr->args[i];
        const ParamAST* param = funcType->params[i];

        TypeAST* argType = resolveExprWithTarget(arg, param->type, ctx);
        if (!argType || argType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }

        if (arg->valueState == ValueState::Err) {
            hasErrArg = true;
        }

        if (arg->valueState == ValueState::Err && !isFallibleType(param->type)) {
            setExprError(expr, ctx, arg, DiagCode::E3003,
                         "cannot pass err to non-fallible parameter");
            return ctx.getUnknownType();
        }

        if (arg->valueState == ValueState::Nil && !isNullableType(param->type)) {
            setExprError(expr, ctx, arg, DiagCode::E3003,
                         "cannot pass nil to non-nullable parameter");
            return ctx.getUnknownType();
        }
    }

    // ─── Step 5: Propagate value state ──────────────────────────────────────
    ValueState state;
    if (hasErrArg && isFallibleType(funcType->returnType)) {
        state = ValueState::Err;
    } else if (isNullableType(funcType->returnType) || isFallibleType(funcType->returnType)) {
        state = ValueState::Unknown;
    } else if (!funcType->returnType) {
        state = ValueState::None;
    } else {
        state = ValueState::Definite;
    }

    setExprResult(expr, funcType->returnType, state);
    return funcType->returnType;
}

// =============================================================================
// resolveIntrinsicCallExpr
// =============================================================================

TypeAST* resolveIntrinsicCallExpr(IntrinsicCallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    auto& registry = IntrinsicRegistry::getInstance(ctx.pool);

    const IntrinsicInfo* info = registry.getIntrinsicInfo(expr->intrinsicName);
    if (!info) {
        setExprError(expr, ctx, expr, DiagCode::E3101,
                     "unknown intrinsic '#" + ctx.pool.lookup(expr->intrinsicName) + "'");
        return ctx.getUnknownType();
    }

    if (!registry.validateArgCount(expr->intrinsicName, expr->args.size())) {
        setExprError(expr, ctx, expr, DiagCode::E3001,
                     "wrong number of arguments for intrinsic '#" +
                     ctx.pool.lookup(expr->intrinsicName) + "'");
        return ctx.getUnknownType();
    }

    // Resolve each argument (no target type for intrinsic args)
    for (ExprAST* arg : expr->args) {
        TypeAST* argType = resolveExpr(arg, ctx);
        if (!argType || argType->isa<UnknownTypeAST>()) {
            setExprError(expr, ctx, arg, DiagCode::E3003,
                         "argument to intrinsic '#" + ctx.pool.lookup(expr->intrinsicName) +
                         "' has unknown type");
            return ctx.getUnknownType();
        }
    }

    if (!registry.validateIntrinsicCall(expr, ctx)) {
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    if (info->isValid()) {
        expr->intrinsicID = info->id;
    }

    const TypeAST* resultType = registry.getIntrinsicReturnType(expr, targetType, ctx);
    ValueState state = registry.getIntrinsicValueState(expr, ctx);
    setExprResult(expr, const_cast<TypeAST*>(resultType), state);
    return const_cast<TypeAST*>(resultType);
}

// =============================================================================
// resolveIndexExpr
// =============================================================================

TypeAST* resolveIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve target ─────────────────────────────────────────────
    TypeAST* targetTypeAst = resolveExpr(expr->target, ctx);
    if (!targetTypeAst || targetTypeAst->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->target, DiagCode::E3003, "index target has unknown type");
        return ctx.getUnknownType();
    }

    if (!targetTypeAst->isa<ArrayTypeAST>()) {
        setExprError(expr, ctx, expr->target, DiagCode::E3003,
                     "indexing requires an array target type, got " +
                     debug::typeToString(targetTypeAst, ctx.pool));
        return ctx.getUnknownType();
    }

    const ArrayTypeAST* arrayType = targetTypeAst->as<ArrayTypeAST>();

    // ─── Step 2: Resolve index against int type ─────────────────────────────
    PrimitiveTypeAST* intType = ctx.getIntType();
    TypeAST* indexType = resolveExprWithTarget(expr->index, intType, ctx);
    if (!indexType || indexType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // ─── Step 3: Propagate value state ──────────────────────────────────────
    ValueState state;
    if (isNullableType(arrayType->element) || isFallibleType(arrayType->element)) {
        state = ValueState::Unknown;
    } else {
        state = ValueState::Definite;
    }

    setExprResult(expr, arrayType->element, state);
    return arrayType->element;
}

// =============================================================================
// resolveSliceExpr
// =============================================================================

TypeAST* resolveSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve target ─────────────────────────────────────────────
    TypeAST* targetTypeAst = resolveExpr(expr->target, ctx);
    if (!targetTypeAst || targetTypeAst->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->target, DiagCode::E3003, "slice target has unknown type");
        return ctx.getUnknownType();
    }

    if (!targetTypeAst->isa<ArrayTypeAST>()) {
        setExprError(expr, ctx, expr->target, DiagCode::E3003,
                     "slicing requires an array target type, got " +
                     debug::typeToString(targetTypeAst, ctx.pool));
        return ctx.getUnknownType();
    }

    const ArrayTypeAST* arrayType = targetTypeAst->as<ArrayTypeAST>();

    // ─── Step 2: Resolve start bound against int type ──────────────────────
    if (expr->start) {
        PrimitiveTypeAST* intType = ctx.getIntType();
        TypeAST* startType = resolveExprWithTarget(expr->start, intType, ctx);
        if (!startType || startType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }
    }

    // ─── Step 3: Resolve end bound against int type ────────────────────────
    if (expr->end) {
        PrimitiveTypeAST* intType = ctx.getIntType();
        TypeAST* endType = resolveExprWithTarget(expr->end, intType, ctx);
        if (!endType || endType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
            return ctx.getUnknownType();
        }
    }

    // ─── Step 4: Propagate value state ──────────────────────────────────────
    ValueState state;
    if (isNullableType(arrayType->element) || isFallibleType(arrayType->element)) {
        state = ValueState::Unknown;
    } else {
        state = ValueState::Definite;
    }

    // Result is always a slice (cached)
    ArrayTypeAST* sliceType = ctx.getArrayType(ArrayKind::Slice, 0, arrayType->element);
    setExprResult(expr, sliceType, state);
    return sliceType;
}

// =============================================================================
// resolveFieldAccessExpr
// =============================================================================

TypeAST* resolveFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve object ─────────────────────────────────────────────
    TypeAST* objectType = resolveExpr(expr->object, ctx);
    if (!objectType || objectType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003, "object has unknown type");
        return ctx.getUnknownType();
    }

    if (expr->object->valueState == ValueState::Nil) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "cannot access field on nil. Use `?.` for nullable access.");
        return ctx.getUnknownType();
    }

    if (expr->object->valueState == ValueState::Err) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "cannot access field on err. Use `??` to handle err first.");
        return ctx.getUnknownType();
    }

    if (isNullableType(objectType)) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "cannot access field on nullable type. Use `?.` for nullable access.");
        return ctx.getUnknownType();
    }

    if (isFallibleType(objectType)) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "cannot access field on fallible type. Use `??` to handle err first.");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Handle generic type ────────────────────────────────────────
    if (objectType->isa<NamedTypeAST>()) {
        const NamedTypeAST* namedType = objectType->as<NamedTypeAST>();

        if (ctx.isGenericParam(namedType->name)) {
            if (!isFieldAccessibleOnGenericType(objectType, expr->fieldName, ctx)) {
                setExprError(expr, ctx, expr, DiagCode::E2210,
                             "field '" + ctx.pool.lookup(expr->fieldName) +
                             "' is not accessible on generic type '" +
                             ctx.pool.lookup(namedType->name) +
                             "' (no trait constraint provides this field)");
                return ctx.getUnknownType();
            }

            const TypeAST* fieldType = getFieldTypeOnGenericType(objectType, expr->fieldName, ctx);
            if (!fieldType) {
                setExprError(expr, ctx, expr, DiagCode::E2001,
                             "field '" + ctx.pool.lookup(expr->fieldName) +
                             "' has no type information in generic constraints");
                return ctx.getUnknownType();
            }

            ValueState state = (isNullableType(fieldType) || isFallibleType(fieldType))
                               ? ValueState::Unknown : ValueState::Definite;
            setExprResult(expr, const_cast<TypeAST*>(fieldType), state);
            return const_cast<TypeAST*>(fieldType);
        }
    }

    // ─── Step 3: Look up the type declaration ──────────────────────────────
    if (!objectType->isa<NamedTypeAST>()) {
        setExprError(expr, ctx, expr->object, DiagCode::E2002,
                     "field access requires a struct or enum type, got " +
                     debug::typeToString(objectType, ctx.pool));
        return ctx.getUnknownType();
    }

    const NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
    const TypeDeclAST* typeDecl = ctx.lookupType(namedType->name);
    if (!typeDecl) {
        setExprError(expr, ctx, expr, DiagCode::E2002,
                     "undefined type '" + ctx.pool.lookup(namedType->name) + "'");
        return ctx.getUnknownType();
    }

    // ─── Step 4: Handle struct type ─────────────────────────────────────────
    if (typeDecl->isa<StructDeclAST>()) {
        const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

        for (const FieldDeclAST* f : structDecl->fields) {
            if (f->name == expr->fieldName) {
                ValueState state = (isNullableType(f->type) || isFallibleType(f->type))
                                   ? ValueState::Unknown : ValueState::Definite;
                setExprResult(expr, f->type, state);
                return f->type;
            }
        }

        setExprError(expr, ctx, expr, DiagCode::E2001,
                     "struct '" + ctx.pool.lookup(structDecl->name) +
                     "' has no field named '" + ctx.pool.lookup(expr->fieldName) + "'");
        return ctx.getUnknownType();
    }

    // ─── Step 5: Handle enum type ───────────────────────────────────────────
    if (typeDecl->isa<EnumDeclAST>()) {
        const EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();

        for (const EnumVariantAST* v : enumDecl->variants) {
            if (v->name == expr->fieldName) {
                setExprResult(expr, ctx.getNamedType(enumDecl->name), ValueState::Definite);
                return ctx.getNamedType(enumDecl->name);
            }
        }

        setExprError(expr, ctx, expr, DiagCode::E2001,
                     "enum '" + ctx.pool.lookup(enumDecl->name) +
                     "' has no variant named '" + ctx.pool.lookup(expr->fieldName) + "'");
        return ctx.getUnknownType();
    }

    setExprError(expr, ctx, expr, DiagCode::E2002,
                 "field access on unsupported type");
    return ctx.getUnknownType();
}

// =============================================================================
// resolveModuleAccessExpr
// =============================================================================

TypeAST* resolveModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Look up the module alias ───────────────────────────────────
    ModuleAST* module = ctx.lookupImport(expr->moduleName);
    if (!module) {
        setExprError(expr, ctx, expr, DiagCode::E2001,
                     "undefined module alias '" + ctx.pool.lookup(expr->moduleName) + "'");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Get the module's table ────────────────────────────────────
    ModuleTable* table = ctx.findModuleTable(module);
    if (!table) {
        setExprError(expr, ctx, expr, DiagCode::E2001,
                     "module '" + ctx.pool.lookup(expr->moduleName) + "' has not been analyzed");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Look up the member ────────────────────────────────────────
    auto it = table->values.find(expr->memberName);
    if (it == table->values.end()) {
        setExprError(expr, ctx, expr, DiagCode::E2001,
                     "module '" + ctx.pool.lookup(expr->moduleName) +
                     "' has no exported member '" + ctx.pool.lookup(expr->memberName) + "'");
        return ctx.getUnknownType();
    }

    const ValueDeclAST* decl = it->second;

    // ─── Step 4: Mark as module member (read-only) ────────────────────────
    expr->isModuleMember = true;

    // ─── Step 5: Check generic arguments if present ────────────────────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "member '" + ctx.pool.lookup(expr->memberName) +
                         "' is not a generic function");
            return ctx.getUnknownType();
        }

        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                setExprError(expr, ctx, expr, DiagCode::E3002,
                             "invalid generic argument type for '" +
                             ctx.pool.lookup(expr->memberName) + "'");
                return ctx.getUnknownType();
            }
        }

        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            return ctx.getUnknownType();
        }
    }

    // ─── Step 6: Return the member's type ───────────────────────────────────
    if (!decl->type) {
        setExprError(expr, ctx, expr, DiagCode::E3003,
                     "member '" + ctx.pool.lookup(expr->memberName) +
                     "' has no type information");
        return ctx.getUnknownType();
    }

    ValueState state = (isNullableType(decl->type) || isFallibleType(decl->type))
                       ? ValueState::Unknown : ValueState::Definite;
    setExprResult(expr, decl->type, state);
    return decl->type;
}

// =============================================================================
// resolveNullableChainExpr
// =============================================================================

TypeAST* resolveNullableChainExpr(NullableChainExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (expr->steps.empty()) {
        setExprError(expr, ctx, expr, DiagCode::E3003, "empty nullable chain");
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve base object ────────────────────────────────────────
    TypeAST* currentType = resolveExpr(expr->object, ctx);
    if (!currentType || currentType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003, "base object has unknown type");
        return ctx.getUnknownType();
    }

    // Base must be nullable
    if (!isNullableType(currentType)) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "?. chain requires a nullable base type (T?), got " +
                     debug::typeToString(currentType, ctx.pool));
        return ctx.getUnknownType();
    }

    if (isFallibleType(currentType)) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "?. chain cannot be used on fallible type. Use `??` to handle err first.");
        return ctx.getUnknownType();
    }

    if (expr->object->valueState == ValueState::Err) {
        setExprError(expr, ctx, expr->object, DiagCode::E3003,
                     "?. chain cannot be used on err. Use `??` to handle err first.");
        return ctx.getUnknownType();
    }

    bool hasNilStep = false;

    // ─── Step 2: Walk through each step ────────────────────────────────────
    for (const InternedString& step : expr->steps) {
        if (!currentType || !isNullableType(currentType)) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "?. step requires nullable type, got " +
                         debug::typeToString(currentType, ctx.pool));
            return ctx.getUnknownType();
        }

        const TypeAST* innerType = unwrapNullable(const_cast<TypeAST*>(currentType));
        if (!innerType) {
            setExprError(expr, ctx, expr, DiagCode::E3003, "cannot unwrap nullable type");
            return ctx.getUnknownType();
        }

        if (!innerType->isa<NamedTypeAST>()) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "?. step requires struct or enum type, got " +
                         debug::typeToString(innerType, ctx.pool));
            return ctx.getUnknownType();
        }

        const NamedTypeAST* namedType = innerType->as<NamedTypeAST>();
        const TypeDeclAST* typeDecl = ctx.lookupType(namedType->name);
        if (!typeDecl) {
            setExprError(expr, ctx, expr, DiagCode::E2002,
                         "undefined type '" + ctx.pool.lookup(namedType->name) + "'");
            return ctx.getUnknownType();
        }

        const FieldDeclAST* field = nullptr;
        if (typeDecl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();
            for (const FieldDeclAST* f : structDecl->fields) {
                if (f->name == step) {
                    field = f;
                    break;
                }
            }
        }

        if (!field) {
            setExprError(expr, ctx, expr, DiagCode::E2001,
                         "type has no field named '" + ctx.pool.lookup(step) + "'");
            return ctx.getUnknownType();
        }

        if (!isNullableType(field->type)) {
            setExprError(expr, ctx, expr, DiagCode::E3003,
                         "?. step '" + ctx.pool.lookup(step) +
                         "' must be nullable, got "  +
                         debug::typeToString(field->type, ctx.pool));
            return ctx.getUnknownType();
        }

        currentType = field->type;
    }

    // ─── Step 3: Propagate value state ────────────────────────────────────
    setExprResult(expr, currentType, hasNilStep ? ValueState::Nil : ValueState::Unknown);
    return currentType;
}

// =============================================================================
// resolveNullCoalesceExpr
// =============================================================================

TypeAST* resolveNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve LHS ────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->value, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->value, DiagCode::E3003, "LHS has unknown type");
        return ctx.getUnknownType();
    }

    if (!isNullableType(lhsType) && !isFallibleType(lhsType)) {
        setExprError(expr, ctx, expr->value, DiagCode::E3003,
                     "?? requires nullable or fallible LHS (T?, T!, or T?!)");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Unwrap LHS type ────────────────────────────────────────────
    const TypeAST* lhsInner = lhsType;
    if (isNullableType(lhsInner)) {
        lhsInner = unwrapNullable(const_cast<TypeAST*>(lhsInner));
    }
    if (isFallibleType(lhsInner)) {
        lhsInner = unwrapFallible(const_cast<TypeAST*>(lhsInner));
    }

    if (!lhsInner) {
        setExprError(expr, ctx, expr, DiagCode::E3003, "cannot unwrap LHS type");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Resolve RHS against LHS inner type ────────────────────────
    TypeAST* rhsType = resolveExprWithTarget(expr->fallback, lhsInner, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // ─── Step 4: Propagate value state ─────────────────────────────────────
    ValueState lhsState = expr->value->valueState;
    ValueState rhsState = expr->fallback->valueState;

    ValueState state;
    if (lhsState == ValueState::Nil || lhsState == ValueState::Err) {
        state = rhsState;
    } else if (lhsState == ValueState::Definite) {
        state = ValueState::Definite;
    } else {
        state = ValueState::Unknown;
    }

    setExprResult(expr, rhsType, state);
    return rhsType;
}

// =============================================================================
// resolveAssignExpr
// =============================================================================

TypeAST* resolveAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve LHS ─────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->lhs, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->lhs, DiagCode::E3003, "LHS has unknown type");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Resolve RHS against LHS type ──────────────────────────────
    TypeAST* rhsType = resolveExprWithTarget(expr->rhs, lhsType, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        setExprResult(expr, ctx.getUnknownType(), ValueState::Unknown);
        return ctx.getUnknownType();
    }

    // ─── Step 3: Compound assignment operator validation ───────────────────
    if (expr->op != AssignOp::Assign) {
        bool isArithmetic = false;
        switch (expr->op) {
            case AssignOp::AddAssign:
            case AssignOp::SubAssign:
            case AssignOp::MulAssign:
            case AssignOp::DivAssign:
            case AssignOp::PowAssign:
            case AssignOp::ModAssign:
                isArithmetic = true;
                if (!isNumericType(lhsType)) {
                    setExprError(expr, ctx, expr, DiagCode::E3003,
                                 "arithmetic compound assignment requires numeric type, got " +
                                 debug::typeToString(lhsType, ctx.pool));
                    return ctx.getUnknownType();
                }
                break;

            case AssignOp::BitAndAssign:
            case AssignOp::BitOrAssign:
            case AssignOp::BitXorAssign:
            case AssignOp::ShlAssign:
            case AssignOp::ShrAssign:
                if (!isIntegerType(lhsType)) {
                    setExprError(expr, ctx, expr, DiagCode::E3003,
                                 "bitwise compound assignment requires integer type, got " +
                                 debug::typeToString(lhsType, ctx.pool));
                    return ctx.getUnknownType();
                }
                break;

            default:
                setExprError(expr, ctx, expr, DiagCode::E3003,
                             "unknown compound assignment operator");
                return ctx.getUnknownType();
        }
    }

    setExprResult(expr, lhsType, expr->rhs->valueState);
    return lhsType;
}

// =============================================================================
// resolvePipelineStep
// =============================================================================

TypeAST* resolvePipelineStep(PipelineStepAST* step, const TypeAST* inputType, SemaContext& ctx) {
    if (!step || !inputType) {
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve callable ───────────────────────────────────────────
    TypeAST* callableType = resolveExpr(step->callable, ctx);
    if (!callableType || callableType->isa<UnknownTypeAST>()) {
        setExprError(step->callable, ctx, step->callable, DiagCode::E2003,
                     "pipeline step callable has unknown type");
        return ctx.getUnknownType();
    }

    if (!callableType->isa<FuncTypeAST>()) {
        setExprError(step->callable, ctx, step->callable, DiagCode::E2003,
                     "pipeline step is not a function type, got " +
                     debug::typeToString(callableType, ctx.pool));
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    if (funcType->params.empty()) {
        setExprError(step->callable, ctx, step->callable, DiagCode::E3003,
                     "pipeline step function takes no parameters");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Verify first parameter matches input type ────────────────
    const TypeAST* firstParamType = funcType->params[0]->type;
    if (!isAssignable(firstParamType, inputType, ctx)) {
        setExprError(step->callable, ctx, step->callable, DiagCode::E3003,
                     "pipeline step input mismatch: expected " +
                     debug::typeToString(firstParamType, ctx.pool) +
                     ", got " + debug::typeToString(inputType, ctx.pool));
        return ctx.getUnknownType();
    }

    // ─── Step 3: Return the output type ────────────────────────────────────
    if (!funcType->returnType) {
        setExprError(step->callable, ctx, step->callable, DiagCode::E3003,
                     "pipeline step returns void");
        return ctx.getUnknownType();
    }

    return funcType->returnType;
}

// =============================================================================
// resolvePipelineExpr
// =============================================================================

TypeAST* resolvePipelineExpr(PipelineExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (expr->steps.empty()) {
        setExprError(expr, ctx, expr, DiagCode::E1107, "pipeline has no steps");
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve seed ──────────────────────────────────────────────
    TypeAST* currentType = resolveExpr(expr->seed, ctx);
    if (!currentType || currentType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->seed, DiagCode::E2002, "seed has unknown type");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Walk through each step ────────────────────────────────────
    for (PipelineStepAST* step : expr->steps) {
        currentType = resolvePipelineStep(step, currentType, ctx);
        if (!currentType || currentType->isa<UnknownTypeAST>()) {
            return ctx.getUnknownType();
        }
    }

    setExprResult(expr, currentType, ValueState::Definite);
    return currentType;
}

// =============================================================================
// resolveComposeOperand
// =============================================================================

TypeAST* resolveComposeOperand(ComposeOperandAST* operand, const TypeAST* targetType, SemaContext& ctx) {
    if (!operand) {
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve callable ──────────────────────────────────────────
    TypeAST* callableType = resolveExpr(operand->callable, ctx);
    if (!callableType || callableType->isa<UnknownTypeAST>()) {
        setExprError(operand->callable, ctx, operand->callable, DiagCode::E2003,
                     "composition operand has unknown type");
        return ctx.getUnknownType();
    }

    if (!callableType->isa<FuncTypeAST>()) {
        setExprError(operand->callable, ctx, operand->callable, DiagCode::E2003,
                     "composition operand is not a function type, got " +
                     debug::typeToString(callableType, ctx.pool));
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // ─── Step 2: Verify function has exactly one parameter group ──────────
    if (funcType->isCurried()) {
        setExprError(operand->callable, ctx, operand->callable, DiagCode::E3003,
                     "composition operand must have exactly one parameter group "
                     "(curried functions are not allowed in composition)");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Check generic arguments if present ────────────────────────
    if (!operand->genericArgs.empty()) {
        const FuncDeclAST* funcDecl = resolveCalleeOrError(operand->callable, ctx);
        if (!funcDecl) {
            setExprError(operand->callable, ctx, operand->callable, DiagCode::E2003,
                         "generic arguments applied to non-generic function");
            return ctx.getUnknownType();
        }

        for (const TypeAST* arg : operand->genericArgs) {
            if (!resolveType(arg, ctx)) {
                setExprError(operand->callable, ctx, operand->callable, DiagCode::E3002,
                             "invalid generic argument type");
                return ctx.getUnknownType();
            }
        }

        if (!validateGenericArguments(operand->genericArgs, funcDecl->genericParams,
                                       operand->callable, ctx)) {
            return ctx.getUnknownType();
        }
    }

    return callableType;
}

// =============================================================================
// resolveComposeExpr
// =============================================================================

TypeAST* resolveComposeExpr(ComposeExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (expr->operands.empty()) {
        setExprError(expr, ctx, expr, DiagCode::E3003, "composition has no operands");
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve left operand ──────────────────────────────────────
    if (!expr->left || !expr->left->isa<ComposeOperandAST>()) {
        setExprError(expr, ctx, expr->left, DiagCode::E2002, "invalid left operand in composition");
        return ctx.getUnknownType();
    }

    TypeAST* leftType = resolveComposeOperand(expr->left->as<ComposeOperandAST>(), nullptr, ctx);
    if (!leftType || leftType->isa<UnknownTypeAST>()) {
        return ctx.getUnknownType();
    }

    if (!leftType->isa<FuncTypeAST>()) {
        setExprError(expr, ctx, expr->left, DiagCode::E2002, "left operand is not a function");
        return ctx.getUnknownType();
    }

    FuncTypeAST* currentFunc = leftType->as<FuncTypeAST>();

    // ─── Step 2: Walk through right operands ───────────────────────────────
    for (ComposeOperandAST* operand : expr->operands) {
        TypeAST* operandType = resolveComposeOperand(operand, nullptr, ctx);
        if (!operandType || operandType->isa<UnknownTypeAST>()) {
            return ctx.getUnknownType();
        }

        if (!operandType->isa<FuncTypeAST>()) {
            setExprError(operand->callable, ctx, operand->callable, DiagCode::E2002,
                         "operand is not a function");
            return ctx.getUnknownType();
        }

        FuncTypeAST* nextFunc = operandType->as<FuncTypeAST>();

        const TypeAST* prevOutput = currentFunc->returnType;
        const TypeAST* nextInput = nextFunc->params.empty() ? nullptr : nextFunc->params[0]->type;

        if (!prevOutput || !nextInput) {
            setExprError(operand->callable, ctx, operand->callable, DiagCode::E3003,
                         "function input/output mismatch in composition");
            return ctx.getUnknownType();
        }

        if (!isAssignable(nextInput, prevOutput, ctx)) {
            setExprError(operand->callable, ctx, operand->callable, DiagCode::E3003,
                         "composition type mismatch: previous output " +
                         debug::typeToString(prevOutput, ctx.pool) +
                         " is not assignable to next input " +
                         debug::typeToString(nextInput, ctx.pool));
            return ctx.getUnknownType();
        }

        currentFunc = nextFunc;
    }

    setExprResult(expr, currentFunc, ValueState::Definite);
    return currentFunc;
}

// =============================================================================
// resolveAnonFuncExpr
// =============================================================================

TypeAST* resolveAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr->funcType) {
        setExprError(expr, ctx, expr, DiagCode::E2002, "anonymous function has no function type");
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve the function type ──────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(expr->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        return ctx.getUnknownType();
    }

    // ─── Step 2: Analyze parameters ────────────────────────────────────────
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── Step 3: Analyze body ──────────────────────────────────────────────
    if (!expr->body) {
        setExprError(expr, ctx, expr, DiagCode::E3003, "anonymous function has no body");
        return ctx.getUnknownType();
    }

    // ─── Use pushAnonFunction (now exists) ─────────────────────────────────
    ctx.stack.pushAnonFunction(expr, funcType, expr->loc);

    bool bodyReturns = false;
    if (expr->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(expr->body->as<BlockStmtAST>(), ctx);
    } else if (expr->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(expr->body->as<ReturnStmtAST>(), ctx);
    } else {
        setExprError(expr, ctx, expr, DiagCode::E3003, "anonymous function has invalid body type");
        ctx.stack.pop();
        return ctx.getUnknownType();
    }

    if (bodyReturns && !ctx.stack.returnRequirementsSatisfied()) {
        ctx.error(expr, DiagCode::E3005,
                  "anonymous function has missing nested return");
    }

    ctx.stack.pop();

    // ─── Step 4: Return the function type ──────────────────────────────────
    ValueState state = (isNullableType(funcType) || isFallibleType(funcType))
                       ? ValueState::Unknown : ValueState::Definite;
    setExprResult(expr, funcType, state);
    return funcType;
}

// =============================================================================
// resolveIfExpr
// =============================================================================

TypeAST* resolveIfExpr(IfExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve condition against bool type ──────────────────────
    PrimitiveTypeAST* boolType = ctx.getBoolType();
    TypeAST* condType = resolveExprWithTarget(expr->condition, boolType, ctx);
    if (!condType || condType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        return ctx.getUnknownType();
    }

    // ─── Step 2: Resolve branches ───────────────────────────────────────────
    TypeAST* thenType = resolveExpr(expr->thenBranch, ctx);
    if (!thenType || thenType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->thenBranch, DiagCode::E3003, "then branch has unknown type");
        return ctx.getUnknownType();
    }

    TypeAST* elseType = resolveExpr(expr->elseBranch, ctx);
    if (!elseType || elseType->isa<UnknownTypeAST>()) {
        setExprError(expr, ctx, expr->elseBranch, DiagCode::E3003, "else branch has unknown type");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Check branch types are compatible ────────────────────────
    if (!isAssignable(thenType, elseType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "if expression branches have incompatible types: then ",
                  debug::typeToString(thenType, ctx.pool),
                  ", else ", debug::typeToString(elseType, ctx.pool));
        return ctx.getUnknownType();
    }

    // ─── Step 4: Propagate value state ──────────────────────────────────────
    ValueState state;
    if (thenType->isa<UnknownTypeAST>() || elseType->isa<UnknownTypeAST>()) {
        state = ValueState::Unknown;
    } else if (isNullableType(thenType) || isNullableType(elseType) ||
               isFallibleType(thenType) || isFallibleType(elseType)) {
        state = ValueState::Unknown;
    } else {
        state = ValueState::Definite;
    }

    setExprResult(expr, thenType, state);
    return thenType;
}

// =============================================================================
// resolveRangeExpr
// =============================================================================

TypeAST* resolveRangeExpr(RangeExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve lower bound ────────────────────────────────────────
    // Range bounds must be numeric - use int as default target
    PrimitiveTypeAST* numericType = ctx.getIntType();
    TypeAST* loType = resolveExprWithTarget(expr->lo, numericType, ctx);
    if (!loType || loType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        return ctx.getUnknownType();
    }

    // ─── Step 2: Resolve upper bound ────────────────────────────────────────
    TypeAST* hiType = resolveExprWithTarget(expr->hi, numericType, ctx);
    if (!hiType || hiType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        return ctx.getUnknownType();
    }

    // ─── Step 3: Validate bounds are same type ─────────────────────────────
    if (!typesEqual(loType, hiType)) {
        ctx.error(expr, DiagCode::E3003,
                  "range bounds must be the same type, got ",
                  debug::typeToString(loType, ctx.pool), " and ",
                  debug::typeToString(hiType, ctx.pool));
        return ctx.getUnknownType();
    }

    setExprResult(expr, loType, ValueState::Definite);
    return loType;
}

} // namespace sema