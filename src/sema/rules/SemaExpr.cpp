/// @file SemaExpr.cpp
/// @brief Implements Sema.hpp's "EXPRESSIONS - Type Resolution" section.
///
/// ============================================================================
/// DESIGN PHILOSOPHY: Resolve to Type, Not Validate Against Target
/// ============================================================================
/// 
/// ─── Core Idea ────────────────────────────────────────────────────────────────
/// 
/// Instead of validating expressions against a target type (which requires
/// knowing the target type upfront), we RESOLVE the expression to its type.
/// 
/// This is a cleaner design because:
///   1. Lucid requires explicit types - every expression has a known type
///   2. We don't need to pass target types around
///   3. Expressions like array literals and switch subjects can be resolved
///   4. The caller can then check assignability if needed
/// 
/// ─── The `resolveExpr` Function ─────────────────────────────────────────────
/// 
/// `resolveExpr(expr, ctx)` returns the type of the expression.
/// 
///   - On success: sets expr->resolvedType to the resolved type
///   - On failure: sets expr->resolvedType to UnknownTypeAST
///   - Always returns a TypeAST* (never nullptr)
/// 
/// ─── Example Flow ──────────────────────────────────────────────────────────
/// 
/// For `let x int = 5 + 3`:
///   1. resolveVarDecl resolves the type of `5 + 3`:
///      - resolveBinaryExpr resolves left `5` → int
///      - resolveBinaryExpr resolves right `3` → int
///      - resolveBinaryExpr determines result → int
///   2. resolveVarDecl checks assignability: int is assignable to int ✅
/// 
/// For `switch x`:
///   1. resolveSwitchStmt resolves `x`:
///      - resolveIdentifierExpr looks up `x` → int
///      - Check: isValidSwitchType(int) → ✅
///   2. Continue with switch validation
/// 
/// ─── Why This Works ─────────────────────────────────────────────────────────
/// 
/// Since Lucid requires explicit type annotations, every expression's type
/// is either:
///   - Explicitly declared (variables, parameters, fields)
///   - Known from the expression itself (literals)
///   - Computed from sub-expressions (binary ops, calls)
/// 
/// There is no "type inference" - we just follow the AST nodes to find the type.
/// 
/// ============================================================================
/// ERROR RECOVERY: UnknownTypeAST
/// ============================================================================
/// 
/// When an expression cannot be resolved, we set its type to UnknownTypeAST.
/// This allows analysis to continue without aborting the entire compilation.
/// 
/// UnknownTypeAST propagates through expressions:
///   - `x + y` where `x` is unknown → result is unknown
///   - `f(x)` where `x` is unknown → result is unknown
/// 
/// This prevents cascading errors where one invalid expression causes many
/// unrelated errors.
/// 
/// ============================================================================
/// NULLABLE VS FALLIBLE: DESIGN RATIONALE
/// ============================================================================
/// 
/// Both nullable (`T?`) and fallible (`T!`) types are resolved and propagated.
/// The resolver determines the type and the value state of each expression.
/// 
/// ─── Value State Propagation ──────────────────────────────────────────────
/// 
/// Each expression has a ValueState:
///   - Definite: Produces a definite value (T)
///   - Nil: Produces nil (T?)
///   - Err: Produces err (T!)
///   - Unknown: Unknown at compile-time
/// 
/// The resolver propagates value states through expressions.

#include "../Sema.hpp"

#include <unordered_set>
#include <optional>

namespace sema {

// =============================================================================
// resolveExpr - Dispatch
// =============================================================================

/// @brief Resolve the type of an expression.
TypeAST* resolveExpr(ExprAST* expr, SemaContext& ctx) {
    if (!expr) {
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    TypeAST* result = nullptr;

    switch (expr->kind) {
        case ASTKind::LiteralExpr:       result = resolveLiteralExpr(expr->as<LiteralExprAST>(), ctx); break;
        case ASTKind::IdentifierExpr:    result = resolveIdentifierExpr(expr->as<IdentifierExprAST>(), ctx); break;
        case ASTKind::ArrayLiteralExpr:  result = resolveArrayLiteralExpr(expr->as<ArrayLiteralExprAST>(), ctx); break;
        case ASTKind::StructLiteralExpr: result = resolveStructLiteralExpr(expr->as<StructLiteralExprAST>(), ctx); break;
        case ASTKind::BinaryExpr:        result = resolveBinaryExpr(expr->as<BinaryExprAST>(), ctx); break;
        case ASTKind::UnaryExpr:         result = resolveUnaryExpr(expr->as<UnaryExprAST>(), ctx); break;
        case ASTKind::CallExpr:          result = resolveCallExpr(expr->as<CallExprAST>(), ctx); break;
        case ASTKind::IntrinsicCallExpr: result = resolveIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>(), ctx); break;
        case ASTKind::IndexExpr:         result = resolveIndexExpr(expr->as<IndexExprAST>(), ctx); break;
        case ASTKind::SliceExpr:         result = resolveSliceExpr(expr->as<SliceExprAST>(), ctx); break;
        case ASTKind::FieldAccessExpr:   result = resolveFieldAccessExpr(expr->as<FieldAccessExprAST>(), ctx); break;
        case ASTKind::ModuleAccessExpr:  result = resolveModuleAccessExpr(expr->as<ModuleAccessExprAST>(), ctx); break;
        case ASTKind::NullableChainExpr: result = resolveNullableChainExpr(expr->as<NullableChainExprAST>(), ctx); break;
        case ASTKind::NullCoalesceExpr:  result = resolveNullCoalesceExpr(expr->as<NullCoalesceExprAST>(), ctx); break;
        case ASTKind::AssignExpr:        result = resolveAssignExpr(expr->as<AssignExprAST>(), ctx); break;
        case ASTKind::PipelineExpr:      result = resolvePipelineExpr(expr->as<PipelineExprAST>(), ctx); break;
        case ASTKind::ComposeExpr:       result = resolveComposeExpr(expr->as<ComposeExprAST>(), ctx); break;
        case ASTKind::AnonFuncExpr:      result = resolveAnonFuncExpr(expr->as<AnonFuncExprAST>(), ctx); break;
        case ASTKind::IfExpr:            result = resolveIfExpr(expr->as<IfExprAST>(), ctx); break;
        case ASTKind::RangeExpr:         result = resolveRangeExpr(expr->as<RangeExprAST>(), ctx); break;
        default:
            ctx.error(expr, DiagCode::E3003, "unsupported expression kind");
            result = ctx.arena().makeType<UnknownTypeAST>();
            break;
    }

    // Store the result on the expression
    expr->resolvedType = result;
    if (!result || result->isa<UnknownTypeAST>()) {
        expr->valueState = ValueState::Unknown;
        return result;
    }

    return result;
}

// =============================================================================
// checkExprAssignable - Convenience Wrapper
// =============================================================================

bool checkExprAssignable(ExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr || !targetType) return false;

    TypeAST* resolvedType = resolveExpr(expr, ctx);
    if (!resolvedType || resolvedType->isa<UnknownTypeAST>()) {
        ctx.error(expr, DiagCode::E2002, "expression has unknown type");
        return false;
    }

    if (!isAssignable(targetType, resolvedType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "type mismatch: expected ", debug::typeToString(targetType, ctx.pool()),
                  ", got ", debug::typeToString(resolvedType, ctx.pool()));
        return false;
    }

    return true;
}

// =============================================================================
// resolveLiteralExpr
// =============================================================================

TypeAST* resolveLiteralExpr(LiteralExprAST* expr, SemaContext& ctx) {
    switch (expr->kind) {
        case LiteralKind::True:
        case LiteralKind::False:
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);

        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary:
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Int);

        case LiteralKind::Float:
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Float);

        case LiteralKind::String:
        case LiteralKind::RawString:
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::String);

        case LiteralKind::Char:
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Char);

        case LiteralKind::Nil:
            expr->valueState = ValueState::Nil;
            // Nil resolves to UnknownTypeAST - its type depends on context
            return ctx.arena().makeType<UnknownTypeAST>();

        case LiteralKind::Err:
            expr->valueState = ValueState::Err;
            // Err resolves to UnknownTypeAST - its type depends on context
            return ctx.arena().makeType<UnknownTypeAST>();

        default:
            ctx.error(expr, DiagCode::E3003, "unknown literal kind");
            return ctx.arena().makeType<UnknownTypeAST>();
    }
}

// =============================================================================
// resolveIdentifierExpr
// =============================================================================

TypeAST* resolveIdentifierExpr(IdentifierExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Check if this is a generic parameter ─────────────────────
    if (isGenericParam(expr->name, ctx)) {
        ctx.error(expr, DiagCode::E2003,
                  "'", ctx.pool().lookup(expr->name), "' is a generic type parameter, not a value");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Resolve the value declaration ────────────────────────────
    const ValueDeclAST* decl = resolveValueOrError(expr, ctx);
    if (!decl) {
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Propagate value state ─────────────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        expr->valueState = ValueState::Unknown;
    } else if (decl->isa<EnumVariantAST>()) {
        expr->valueState = ValueState::Definite;
    } else if (decl->isa<FuncDeclAST>()) {
        expr->valueState = ValueState::Definite;
    } else if (decl->isa<ParamAST>()) {
        expr->valueState = ValueState::Unknown;
    } else {
        expr->valueState = ValueState::Unknown;
    }

    // ─── Step 4: Handle generic arguments (function instantiation) ────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            ctx.error(expr, DiagCode::E3003,
                      "'", ctx.pool().lookup(expr->name), "' is not a function");
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.error(expr, DiagCode::E3002,
                          "invalid generic argument type for '", 
                          ctx.pool().lookup(expr->name), "'");
                return ctx.arena().makeType<UnknownTypeAST>();
            }
        }

        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 5: Return the declaration's type ─────────────────────────────
    if (!decl->type) {
        ctx.error(expr, DiagCode::E2002,
                  "'", ctx.pool().lookup(expr->name), "' has no type information");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    return decl->type;
}

// =============================================================================
// resolveArrayLiteralExpr
// =============================================================================

TypeAST* resolveArrayLiteralExpr(ArrayLiteralExprAST* expr, SemaContext& ctx) {
    if (expr->elements.empty()) {
        // Empty array - type unknown, needs context
        expr->valueState = ValueState::Definite;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Resolve the type of the first element
    TypeAST* firstType = resolveExpr(expr->elements[0], ctx);
    if (!firstType || firstType->isa<UnknownTypeAST>()) {
        ctx.error(expr, DiagCode::E3003, "array literal element has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Check all elements against the first type
    bool allMatch = true;
    bool hasErr = false;
    bool allDefinite = true;

    for (size_t i = 1; i < expr->elements.size(); ++i) {
        TypeAST* elemType = resolveExpr(expr->elements[i], ctx);
        if (!elemType) {
            allMatch = false;
            continue;
        }

        if (elemType->isa<UnknownTypeAST>()) {
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
        // Try to find a common type (int + float → float, etc.)
        // For now, mark as unknown and report error
        ctx.error(expr, DiagCode::E3003,
                  "array literal contains elements of different types");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Propagate value state
    if (hasErr) {
        expr->valueState = ValueState::Err;
    } else if (allDefinite) {
        expr->valueState = ValueState::Definite;
    } else {
        expr->valueState = ValueState::Unknown;
    }

    // Create an array type with the element type
    return ctx.arena().makeType<ArrayTypeAST>(
        ArrayKind::Dynamic, 0, firstType
    );
}

// =============================================================================
// resolveStructLiteralExpr
// =============================================================================

TypeAST* resolveStructLiteralExpr(StructLiteralExprAST* expr, SemaContext& ctx) {
    // The type is determined by the struct name
    const TypeDeclAST* typeDecl = resolveTypeNameOrError(
        ctx.arena().makeType<NamedTypeAST>(expr->typeName)->as<NamedTypeAST>(), ctx
    );
    if (!typeDecl) {
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.error(expr, DiagCode::E2002,
                  "'", ctx.pool().lookup(expr->typeName), "' is not a struct");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Check generic arguments
    if (!expr->genericArgs.empty()) {
        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.error(expr, DiagCode::E3002, "invalid generic argument type");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }
        }
        // TODO: Check generic argument arity and constraints
    }

    // Track if any field initializer is err
    bool hasErr = false;
    bool allDefinite = true;

    // Map field names to their declaration
    std::unordered_map<InternedString, const FieldDeclAST*> fieldMap;
    for (const FieldDeclAST* field : structDecl->fields) {
        fieldMap[field->name] = field;
    }

    std::unordered_set<InternedString> initializedFields;

    // Validate each field initializer
    for (const FieldInitAST* init : expr->inits) {
        auto it = fieldMap.find(init->name);
        if (it == fieldMap.end()) {
            ctx.error(init, DiagCode::E2001,
                      "struct '", ctx.pool().lookup(structDecl->name),
                      "' has no field named '", ctx.pool().lookup(init->name), "'");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        const FieldDeclAST* field = it->second;

        // Const field validation
        if (field->isConst) {
            if (init->value->isa<LiteralExprAST>()) {
                const LiteralExprAST* literal = init->value->as<LiteralExprAST>();
                if (literal->kind == LiteralKind::Nil || literal->kind == LiteralKind::Err) {
                    ctx.error(init, DiagCode::E3004,
                              "const field '", ctx.pool().lookup(field->name),
                              "' cannot be assigned '", 
                              (literal->kind == LiteralKind::Nil ? "nil" : "err"),
                              "' (const fields must have definite values)");
                    expr->valueState = ValueState::Unknown;
                    return ctx.arena().makeType<UnknownTypeAST>();
                }
            }
        }

        // Check the initializer type
        TypeAST* initType = resolveExpr(init->value, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            ctx.error(init, DiagCode::E3003, "field initializer has unknown type");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!isAssignable(field->type, initType, ctx)) {
            ctx.error(init, DiagCode::E3003,
                      "field '", ctx.pool().lookup(field->name),
                      "' type mismatch: expected ",
                      debug::typeToString(field->type, ctx.pool()),
                      ", got ", debug::typeToString(initType, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (init->value->valueState == ValueState::Err) {
            hasErr = true;
        }
        if (init->value->valueState != ValueState::Definite) {
            allDefinite = false;
        }

        initializedFields.insert(init->name);
    }

    // Check for missing required fields
    for (const FieldDeclAST* field : structDecl->fields) {
        if (initializedFields.find(field->name) != initializedFields.end()) {
            continue;
        }

        if (field->defaultVal) {
            continue;
        }

        if (isNullableType(field->type)) {
            continue;
        }

        if (isFallibleType(field->type)) {
            continue;
        }

        if (field->type->isa<CombinedTypeAST>()) {
            ctx.error(expr, DiagCode::E3002,
                      "combined field '", ctx.pool().lookup(field->name),
                      "' (T?!) must be explicitly initialized (no implicit default)");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        ctx.error(expr, DiagCode::E3002,
                  "field '", ctx.pool().lookup(field->name),
                  "' must be initialized in struct literal (no default value)");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Propagate value state
    if (hasErr) {
        expr->valueState = ValueState::Err;
    } else if (allDefinite && !expr->inits.empty()) {
        expr->valueState = ValueState::Definite;
    } else {
        expr->valueState = ValueState::Unknown;
    }

    // Return the struct type
    return ctx.arena().makeType<NamedTypeAST>(structDecl->name);
}

// =============================================================================
// resolveBinaryExpr
// =============================================================================

TypeAST* resolveBinaryExpr(BinaryExprAST* expr, SemaContext& ctx) {
    TypeAST* leftType = resolveExpr(expr->left, ctx);
    TypeAST* rightType = resolveExpr(expr->right, ctx);

    if (!leftType || leftType->isa<UnknownTypeAST>() ||
        !rightType || rightType->isa<UnknownTypeAST>()) {
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    ValueState leftState = expr->left->valueState;
    ValueState rightState = expr->right->valueState;

    bool leftIsErr = leftState == ValueState::Err;
    bool rightIsErr = rightState == ValueState::Err;
    bool leftIsNil = leftState == ValueState::Nil;
    bool rightIsNil = rightState == ValueState::Nil;

    // ─── Check if we're in an if condition context ─────────────────────────
    if (ctx.contexts.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.contexts.setPendingNarrowing(info);
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }
    }

    // ─── Validate operator-specific rules ──────────────────────────────────
    switch (expr->op) {
        // ─── Arithmetic Operators ──────────────────────────────────────────
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::Mod: {
            // Check for nil operands - never allowed in arithmetic
            if (leftIsNil || rightIsNil) {
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator cannot be used with nil. Use `??` to handle nil first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            // Check for err operands
            if (leftIsErr || rightIsErr) {
                // If either operand is err, the result is err
                // But we need the type to be the numeric type
                if (isNumericType(leftType)) {
                    expr->valueState = ValueState::Err;
                    return leftType;
                }
                if (isNumericType(rightType)) {
                    expr->valueState = ValueState::Err;
                    return rightType;
                }
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator cannot be used with err. Use `??` to handle err first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            // Both operands must be numeric
            if (!isNumericType(leftType) || !isNumericType(rightType)) {
                ctx.error(expr, DiagCode::E3003,
                          "arithmetic operator requires numeric operands");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            // Both operands are definite
            expr->valueState = ValueState::Definite;
            return leftType; // Same type as operands (promoted if needed)
        }

        // ─── Comparison Operators ──────────────────────────────────────────
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge: {
            // Comparison always returns bool
            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }

        // ─── Logical Operators ─────────────────────────────────────────────
        case BinaryOp::And:
        case BinaryOp::Or: {
            // Check for nil operands
            if (leftIsNil || rightIsNil) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator cannot be used with nil");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (leftIsErr || rightIsErr) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator cannot be used with err");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            // Both operands must be bool
            if (!isBoolType(leftType) || !isBoolType(rightType)) {
                ctx.error(expr, DiagCode::E3003,
                          "logical operator requires bool operands");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }

        // ─── Bitwise Operators ─────────────────────────────────────────────
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            // Check for nil operands
            if (leftIsNil || rightIsNil) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator cannot be used with nil");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (leftIsErr || rightIsErr) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator cannot be used with err");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            // Both operands must be integer
            if (!isIntegerType(leftType) || !isIntegerType(rightType)) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise operator requires integer operands");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            expr->valueState = ValueState::Definite;
            return leftType;
        }
    }

    expr->valueState = ValueState::Unknown;
    return ctx.arena().makeType<UnknownTypeAST>();
}

// =============================================================================
// resolveUnaryExpr
// =============================================================================

TypeAST* resolveUnaryExpr(UnaryExprAST* expr, SemaContext& ctx) {
    TypeAST* operandType = resolveExpr(expr->operand, ctx);

    if (!operandType || operandType->isa<UnknownTypeAST>()) {
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    ValueState operandState = expr->operand->valueState;
    bool isNil = operandState == ValueState::Nil;
    bool isErr = operandState == ValueState::Err;

    switch (expr->op) {
        // ─── Arithmetic Negation (-x) ─────────────────────────────────────
        case UnaryOp::Neg: {
            if (isNil) {
                ctx.error(expr, DiagCode::E3003,
                          "negation cannot be used with nil. Use `??` to handle nil first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (isErr) {
                expr->valueState = ValueState::Err;
                return operandType;
            }

            if (!isNumericType(operandType)) {
                ctx.error(expr, DiagCode::E3003,
                          "negation requires numeric operand");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            expr->valueState = ValueState::Definite;
            return operandType;
        }

        // ─── Logical Not (not x) ──────────────────────────────────────────
        case UnaryOp::Not: {
            if (isNil) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not cannot be used with nil. Use `??` to handle nil first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (isErr) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not cannot be used with err. Use `??` to handle err first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (!isBoolType(operandType)) {
                ctx.error(expr, DiagCode::E3003,
                          "logical not requires bool operand");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            expr->valueState = ValueState::Definite;
            return ctx.arena().makeType<PrimitiveTypeAST>(PrimitiveKind::Bool);
        }

        // ─── Bitwise Not (~x) ─────────────────────────────────────────────
        case UnaryOp::BitNot: {
            if (isNil) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not cannot be used with nil. Use `??` to handle nil first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (isErr) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not cannot be used with err. Use `??` to handle err first.");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (!isIntegerType(operandType)) {
                ctx.error(expr, DiagCode::E3003,
                          "bitwise not requires integer operand");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            expr->valueState = ValueState::Definite;
            return operandType;
        }
    }

    expr->valueState = ValueState::Unknown;
    return ctx.arena().makeType<UnknownTypeAST>();
}

// =============================================================================
// resolveCallExpr
// =============================================================================

TypeAST* resolveCallExpr(CallExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve callee ─────────────────────────────────────────────
    TypeAST* calleeType = resolveExpr(expr->callee, ctx);
    if (!calleeType || calleeType->isa<UnknownTypeAST>()) {
        ctx.error(expr->callee, DiagCode::E2003, "callee has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Callee must be definite
    if (expr->callee->valueState == ValueState::Nil) {
        ctx.error(expr->callee, DiagCode::E3003,
                  "cannot call nil value. Use `??` to handle nil first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (expr->callee->valueState == ValueState::Err) {
        ctx.error(expr->callee, DiagCode::E3003,
                  "cannot call err value. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Callee must be a function type
    if (!calleeType->isa<FuncTypeAST>()) {
        ctx.error(expr->callee, DiagCode::E2003,
                  "expression is not callable");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    FuncTypeAST* funcType = calleeType->as<FuncTypeAST>();

    // ─── Step 2: Check generic arguments ────────────────────────────────────
    // Only for named function calls
    const FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);
    if (funcDecl) {
        if (!expr->genericArgs.empty()) {
            for (const TypeAST* arg : expr->genericArgs) {
                if (!resolveType(arg, ctx)) {
                    ctx.error(expr, DiagCode::E3002,
                              "invalid generic argument type for '", 
                              ctx.pool().lookup(funcDecl->name), "'");
                    expr->valueState = ValueState::Unknown;
                    return ctx.arena().makeType<UnknownTypeAST>();
                }
            }

            if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }
        } else if (!funcDecl->genericParams.empty()) {
            ctx.error(expr, DiagCode::E2207,
                      "generic function '", ctx.pool().lookup(funcDecl->name),
                      "' requires ", std::to_string(funcDecl->genericParams.size()),
                      " generic argument(s)");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    } else {
        // Function expression - generic arguments not allowed
        if (!expr->genericArgs.empty()) {
            ctx.error(expr, DiagCode::E3003,
                      "generic arguments can only be applied to named function calls");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 3: Check argument count ──────────────────────────────────────
    size_t expectedArgCount = funcType->params.size();
    if (expr->args.size() != expectedArgCount) {
        ctx.error(expr, DiagCode::E3001,
                  "wrong number of arguments: expected ",
                  std::to_string(expectedArgCount), ", found ",
                  std::to_string(expr->args.size()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 4: Check each argument type ──────────────────────────────────
    bool hasErrArg = false;
    for (size_t i = 0; i < expr->args.size(); ++i) {
        ExprAST* arg = expr->args[i];
        const ParamAST* param = funcType->params[i];

        TypeAST* argType = resolveExpr(arg, ctx);
        if (!argType || argType->isa<UnknownTypeAST>()) {
            ctx.error(arg, DiagCode::E3003, "argument has unknown type");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!isAssignable(param->type, argType, ctx)) {
            ctx.error(arg, DiagCode::E3003,
                      "argument type mismatch: expected ",
                      debug::typeToString(param->type, ctx.pool()),
                      ", got ", debug::typeToString(argType, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (arg->valueState == ValueState::Err) {
            hasErrArg = true;
        }

        if (arg->valueState == ValueState::Err && !isFallibleType(param->type)) {
            ctx.error(arg, DiagCode::E3003,
                      "cannot pass err to non-fallible parameter");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (arg->valueState == ValueState::Nil && !isNullableType(param->type)) {
            ctx.error(arg, DiagCode::E3003,
                      "cannot pass nil to non-nullable parameter");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 5: Propagate value state ──────────────────────────────────────
    if (hasErrArg) {
        if (isFallibleType(funcType->returnType)) {
            expr->valueState = ValueState::Err;
        } else {
            expr->valueState = ValueState::Unknown;
        }
    } else {
        if (isNullableType(funcType->returnType)) {
            expr->valueState = ValueState::Unknown;
        } else if (isFallibleType(funcType->returnType)) {
            expr->valueState = ValueState::Unknown;
        } else {
            expr->valueState = ValueState::Definite;
        }
    }

    // ─── Step 6: Return the return type ─────────────────────────────────────
    if (!funcType->returnType) {
        // Void function
        expr->valueState = ValueState::None;
    }

    return funcType->returnType;
}

// =============================================================================
// resolveIntrinsicCallExpr
// =============================================================================

TypeAST* resolveIntrinsicCallExpr(IntrinsicCallExprAST* expr, SemaContext& ctx) {
    auto& registry = IntrinsicRegistry::getInstance(ctx.pool());

    const IntrinsicInfo* info = registry.getIntrinsicInfo(expr->intrinsicName);
    if (!info) {
        ctx.error(expr, DiagCode::E3101,
                  "unknown intrinsic '#", ctx.pool().lookup(expr->intrinsicName), "'");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!registry.validateArgCount(expr->intrinsicName, expr->args.size())) {
        ctx.error(expr, DiagCode::E3001,
                  "wrong number of arguments for intrinsic '#",
                  ctx.pool().lookup(expr->intrinsicName), "'");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Resolve each argument
    for (ExprAST* arg : expr->args) {
        if (!resolveExpr(arg, ctx) || arg->resolvedType->isa<UnknownTypeAST>()) {
            ctx.error(arg, DiagCode::E3003,
                      "argument to intrinsic '#", ctx.pool().lookup(expr->intrinsicName),
                      "' has unknown type");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    if (!registry.validateIntrinsicCall(expr, ctx)) {
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (info->isValid()) {
        expr->intrinsicID = info->id;
    }

    expr->valueState = registry.getIntrinsicValueState(expr, ctx);
    return registry.getIntrinsicReturnType(expr, ctx);
}

// =============================================================================
// resolveIndexExpr
// =============================================================================

TypeAST* resolveIndexExpr(IndexExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve target ─────────────────────────────────────────────
    TypeAST* targetType = resolveExpr(expr->target, ctx);
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003, "index target has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Target must be an array type
    if (!targetType->isa<ArrayTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003,
                  "indexing requires an array target type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    const ArrayTypeAST* arrayType = targetType->as<ArrayTypeAST>();

    // ─── Step 2: Resolve index ──────────────────────────────────────────────
    TypeAST* indexType = resolveExpr(expr->index, ctx);
    if (!indexType || indexType->isa<UnknownTypeAST>()) {
        ctx.error(expr->index, DiagCode::E3003, "index has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Index must be definite integer
    if (isNullableType(indexType)) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be nullable. Use `??` to narrow first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (isFallibleType(indexType)) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be fallible. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (expr->index->valueState == ValueState::Nil) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be nil. Use `??` to handle nil first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (expr->index->valueState == ValueState::Err) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index cannot be err. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!isIntegerType(indexType)) {
        ctx.error(expr->index, DiagCode::E3003,
                  "index must be an integer type, got ",
                  debug::typeToString(indexType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Propagate value state ──────────────────────────────────────
    if (isNullableType(arrayType->element)) {
        expr->valueState = ValueState::Unknown;
    } else if (isFallibleType(arrayType->element)) {
        expr->valueState = ValueState::Unknown;
    } else {
        expr->valueState = ValueState::Definite;
    }

    return arrayType->element;
}

// =============================================================================
// resolveSliceExpr
// =============================================================================

TypeAST* resolveSliceExpr(SliceExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve target ─────────────────────────────────────────────
    TypeAST* targetType = resolveExpr(expr->target, ctx);
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003, "slice target has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!targetType->isa<ArrayTypeAST>()) {
        ctx.error(expr->target, DiagCode::E3003,
                  "slicing requires an array target type, got ",
                  debug::typeToString(targetType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    const ArrayTypeAST* arrayType = targetType->as<ArrayTypeAST>();

    // ─── Step 2: Resolve start bound ────────────────────────────────────────
    if (expr->start) {
        TypeAST* startType = resolveExpr(expr->start, ctx);
        if (!startType || startType->isa<UnknownTypeAST>()) {
            ctx.error(expr->start, DiagCode::E3003, "slice start has unknown type");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (isNullableType(startType)) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be nullable. Use `??` to narrow first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (isFallibleType(startType)) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be fallible. Use `??` to handle err first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (expr->start->valueState == ValueState::Nil) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be nil. Use `??` to handle nil first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (expr->start->valueState == ValueState::Err) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start cannot be err. Use `??` to handle err first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!isIntegerType(startType)) {
            ctx.error(expr->start, DiagCode::E3003,
                      "slice start must be an integer type, got ",
                      debug::typeToString(startType, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 3: Resolve end bound ──────────────────────────────────────────
    if (expr->end) {
        TypeAST* endType = resolveExpr(expr->end, ctx);
        if (!endType || endType->isa<UnknownTypeAST>()) {
            ctx.error(expr->end, DiagCode::E3003, "slice end has unknown type");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (isNullableType(endType)) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be nullable. Use `??` to narrow first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (isFallibleType(endType)) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be fallible. Use `??` to handle err first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (expr->end->valueState == ValueState::Nil) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be nil. Use `??` to handle nil first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (expr->end->valueState == ValueState::Err) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end cannot be err. Use `??` to handle err first.");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!isIntegerType(endType)) {
            ctx.error(expr->end, DiagCode::E3003,
                      "slice end must be an integer type, got ",
                      debug::typeToString(endType, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 4: Propagate value state ──────────────────────────────────────
    if (isNullableType(arrayType->element)) {
        expr->valueState = ValueState::Unknown;
    } else if (isFallibleType(arrayType->element)) {
        expr->valueState = ValueState::Unknown;
    } else {
        expr->valueState = ValueState::Definite;
    }

    // Result is always a slice
    return ctx.arena().makeType<ArrayTypeAST>(
        ArrayKind::Slice, 0, arrayType->element
    );
}

// =============================================================================
// resolveFieldAccessExpr
// =============================================================================

TypeAST* resolveFieldAccessExpr(FieldAccessExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve object ─────────────────────────────────────────────
    TypeAST* objectType = resolveExpr(expr->object, ctx);
    if (!objectType || objectType->isa<UnknownTypeAST>()) {
        ctx.error(expr->object, DiagCode::E3003, "object has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Object must be definite
    if (expr->object->valueState == ValueState::Nil) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on nil. Use `?.` for nullable access.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (expr->object->valueState == ValueState::Err) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on err. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (isNullableType(objectType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on nullable type. Use `?.` for nullable access.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (isFallibleType(objectType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "cannot access field on fallible type. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Handle generic type ────────────────────────────────────────
    if (objectType->isa<NamedTypeAST>()) {
        const NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
        
        if (isGenericParam(namedType->name, ctx)) {
            if (!isFieldAccessibleOnGenericType(objectType, expr->fieldName, ctx)) {
                ctx.error(expr, DiagCode::E2210,
                          "field '", ctx.pool().lookup(expr->fieldName),
                          "' is not accessible on generic type '",
                          ctx.pool().lookup(namedType->name),
                          "' (no trait constraint provides this field)");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            const TypeAST* fieldType = getFieldTypeOnGenericType(objectType, expr->fieldName, ctx);
            if (!fieldType) {
                ctx.error(expr, DiagCode::E2001,
                          "field '", ctx.pool().lookup(expr->fieldName),
                          "' has no type information in generic constraints");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }

            if (isNullableType(fieldType)) {
                expr->valueState = ValueState::Unknown;
            } else if (isFallibleType(fieldType)) {
                expr->valueState = ValueState::Unknown;
            } else {
                expr->valueState = ValueState::Definite;
            }

            return const_cast<TypeAST*>(fieldType);
        }
    }

    // ─── Step 3: Look up the type declaration ──────────────────────────────
    if (!objectType->isa<NamedTypeAST>()) {
        ctx.error(expr->object, DiagCode::E2002,
                  "field access requires a struct or enum type, got ",
                  debug::typeToString(objectType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    const NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
    const TypeDeclAST* typeDecl = lookupType(namedType->name, ctx);
    if (!typeDecl) {
        ctx.error(expr, DiagCode::E2002,
                  "undefined type '", ctx.pool().lookup(namedType->name), "'");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 4: Handle struct type ─────────────────────────────────────────
    if (typeDecl->isa<StructDeclAST>()) {
        const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

        const FieldDeclAST* field = nullptr;
        for (const FieldDeclAST* f : structDecl->fields) {
            if (f->name == expr->fieldName) {
                field = f;
                break;
            }
        }

        if (!field) {
            ctx.error(expr, DiagCode::E2001,
                      "struct '", ctx.pool().lookup(structDecl->name),
                      "' has no field named '", ctx.pool().lookup(expr->fieldName), "'");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (isNullableType(field->type)) {
            expr->valueState = ValueState::Unknown;
        } else if (isFallibleType(field->type)) {
            expr->valueState = ValueState::Unknown;
        } else {
            expr->valueState = ValueState::Definite;
        }

        return field->type;
    }

    // ─── Step 5: Handle enum type ───────────────────────────────────────────
    if (typeDecl->isa<EnumDeclAST>()) {
        const EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();

        const EnumVariantAST* variant = nullptr;
        for (const EnumVariantAST* v : enumDecl->variants) {
            if (v->name == expr->fieldName) {
                variant = v;
                break;
            }
        }

        if (!variant) {
            ctx.error(expr, DiagCode::E2001,
                      "enum '", ctx.pool().lookup(enumDecl->name),
                      "' has no variant named '", ctx.pool().lookup(expr->fieldName), "'");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        expr->valueState = ValueState::Definite;
        return ctx.arena().makeType<NamedTypeAST>(enumDecl->name);
    }

    ctx.error(expr, DiagCode::E2002,
              "field access on unsupported type: ",
              debug::typeToString(objectType, ctx.pool()));
    expr->valueState = ValueState::Unknown;
    return ctx.arena().makeType<UnknownTypeAST>();
}

// =============================================================================
// resolveModuleAccessExpr
// =============================================================================

TypeAST* resolveModuleAccessExpr(ModuleAccessExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Look up the module alias ───────────────────────────────────
    ModuleAST* module = ctx.symbols.lookupImport(expr->moduleName);
    if (!module) {
        ctx.error(expr, DiagCode::E2001,
                  "undefined module alias '", ctx.pool().lookup(expr->moduleName), "'");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Get the module's table ────────────────────────────────────
    ModuleTable* table = ctx.symbols.findModuleTable(module);
    if (!table) {
        ctx.error(expr, DiagCode::E2001,
                  "module '", ctx.pool().lookup(expr->moduleName), "' has not been analyzed");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Look up the member ────────────────────────────────────────
    auto it = table->values.find(expr->memberName);
    if (it == table->values.end()) {
        ctx.error(expr, DiagCode::E2001,
                  "module '", ctx.pool().lookup(expr->moduleName),
                  "' has no exported member '", ctx.pool().lookup(expr->memberName), "'");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    const ValueDeclAST* decl = it->second;

    // ─── Step 4: Mark as module member (read-only) ────────────────────────
    expr->isModuleMember = true;

    // ─── Step 5: Check generic arguments if present ────────────────────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            ctx.error(expr, DiagCode::E3003,
                      "member '", ctx.pool().lookup(expr->memberName), 
                      "' is not a generic function");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        for (const TypeAST* arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.error(expr, DiagCode::E3002,
                          "invalid generic argument type for '", 
                          ctx.pool().lookup(expr->memberName), "'");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
            }
        }

        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 6: Return the member's type ───────────────────────────────────
    if (!decl->type) {
        ctx.error(expr, DiagCode::E3003,
                  "member '", ctx.pool().lookup(expr->memberName),
                  "' has no type information");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (isNullableType(decl->type)) {
        expr->valueState = ValueState::Unknown;
    } else if (isFallibleType(decl->type)) {
        expr->valueState = ValueState::Unknown;
    } else {
        expr->valueState = ValueState::Definite;
    }

    return decl->type;
}

// =============================================================================
// resolveNullableChainExpr
// =============================================================================

TypeAST* resolveNullableChainExpr(NullableChainExprAST* expr, SemaContext& ctx) {
    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E3003, "empty nullable chain");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 1: Resolve base object ────────────────────────────────────────
    TypeAST* currentType = resolveExpr(expr->object, ctx);
    if (!currentType || currentType->isa<UnknownTypeAST>()) {
        ctx.error(expr->object, DiagCode::E3003, "base object has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // Base must be nullable
    if (!isNullableType(currentType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "?. chain requires a nullable base type (T?), got ",
                  debug::typeToString(currentType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (isFallibleType(currentType)) {
        ctx.error(expr->object, DiagCode::E3003,
                  "?. chain cannot be used on fallible type. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (expr->object->valueState == ValueState::Err) {
        ctx.error(expr->object, DiagCode::E3003,
                  "?. chain cannot be used on err. Use `??` to handle err first.");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    bool hasNilStep = false;

    // ─── Step 2: Walk through each step ────────────────────────────────────
    for (const InternedString& step : expr->steps) {
        if (!currentType || !isNullableType(currentType)) {
            ctx.error(expr, DiagCode::E3003,
                      "?. step requires nullable type, got ",
                      debug::typeToString(currentType, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        const TypeAST* innerType = unwrapNullable(const_cast<TypeAST*>(currentType));
        if (!innerType) {
            ctx.error(expr, DiagCode::E3003, "cannot unwrap nullable type");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!innerType->isa<NamedTypeAST>()) {
            ctx.error(expr, DiagCode::E3003,
                      "?. step requires struct or enum type, got ",
                      debug::typeToString(innerType, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        const NamedTypeAST* namedType = innerType->as<NamedTypeAST>();
        const TypeDeclAST* typeDecl = lookupType(namedType->name, ctx);
        if (!typeDecl) {
            ctx.error(expr, DiagCode::E2002,
                      "undefined type '", ctx.pool().lookup(namedType->name), "'");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
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
            ctx.error(expr, DiagCode::E2001,
                      "type has no field named '", ctx.pool().lookup(step), "'");
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!isNullableType(field->type)) {
            ctx.error(expr, DiagCode::E3003,
                      "?. step '", ctx.pool().lookup(step),
                      "' must be nullable, got ",
                      debug::typeToString(field->type, ctx.pool()));
            expr->valueState = ValueState::Unknown;
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        currentType = field->type;
    }

    // ─── Step 3: Propagate value state ────────────────────────────────────
    if (hasNilStep) {
        expr->valueState = ValueState::Nil;
    } else {
        expr->valueState = ValueState::Unknown;
    }

    return currentType;
}

// =============================================================================
// resolveNullCoalesceExpr
// =============================================================================

TypeAST* resolveNullCoalesceExpr(NullCoalesceExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve LHS ────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->value, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        ctx.error(expr->value, DiagCode::E3003, "LHS has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // LHS must be nullable or fallible
    if (!isNullableType(lhsType) && !isFallibleType(lhsType)) {
        ctx.error(expr->value, DiagCode::E3003,
                  "?? requires nullable or fallible LHS (T?, T!, or T?!)");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
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
        ctx.error(expr, DiagCode::E3003, "cannot unwrap LHS type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Resolve RHS ────────────────────────────────────────────────
    TypeAST* rhsType = resolveExpr(expr->fallback, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        ctx.error(expr->fallback, DiagCode::E3003, "RHS has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!isAssignable(lhsInner, rhsType, ctx)) {
        ctx.error(expr->fallback, DiagCode::E3003,
                  "fallback type mismatch: expected ",
                  debug::typeToString(lhsInner, ctx.pool()),
                  ", got ", debug::typeToString(rhsType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 4: Propagate value state ─────────────────────────────────────
    ValueState lhsState = expr->value->valueState;
    ValueState rhsState = expr->fallback->valueState;

    if (lhsState == ValueState::Nil || lhsState == ValueState::Err) {
        expr->valueState = rhsState;
    } else if (lhsState == ValueState::Definite) {
        expr->valueState = ValueState::Definite;
    } else {
        expr->valueState = ValueState::Unknown;
    }

    return rhsType;
}

// =============================================================================
// resolveAssignExpr
// =============================================================================

TypeAST* resolveAssignExpr(AssignExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve LHS ─────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->lhs, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        ctx.error(expr->lhs, DiagCode::E3003, "LHS has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Resolve RHS ─────────────────────────────────────────────────
    TypeAST* rhsType = resolveExpr(expr->rhs, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        ctx.error(expr->rhs, DiagCode::E3003, "RHS has unknown type");
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Check assignability ────────────────────────────────────────
    if (!isAssignable(lhsType, rhsType, ctx)) {
        ctx.error(expr->rhs, DiagCode::E3003,
                  "type mismatch: expected ",
                  debug::typeToString(lhsType, ctx.pool()),
                  ", got ", debug::typeToString(rhsType, ctx.pool()));
        expr->valueState = ValueState::Unknown;
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 4: Compound assignment operator validation ───────────────────
    if (expr->op != AssignOp::Assign) {
        switch (expr->op) {
            case AssignOp::AddAssign:
            case AssignOp::SubAssign:
            case AssignOp::MulAssign:
            case AssignOp::DivAssign:
            case AssignOp::PowAssign:
            case AssignOp::ModAssign:
                if (!isNumericType(lhsType)) {
                    ctx.error(expr, DiagCode::E3003,
                              "arithmetic compound assignment requires numeric type, got ",
                              debug::typeToString(lhsType, ctx.pool()));
                    expr->valueState = ValueState::Unknown;
                    return ctx.arena().makeType<UnknownTypeAST>();
                }
                break;

            case AssignOp::BitAndAssign:
            case AssignOp::BitOrAssign:
            case AssignOp::BitXorAssign:
            case AssignOp::ShlAssign:
            case AssignOp::ShrAssign:
                if (!isIntegerType(lhsType)) {
                    ctx.error(expr, DiagCode::E3003,
                              "bitwise compound assignment requires integer type, got ",
                              debug::typeToString(lhsType, ctx.pool()));
                    expr->valueState = ValueState::Unknown;
                    return ctx.arena().makeType<UnknownTypeAST>();
                }
                break;

            default:
                ctx.error(expr, DiagCode::E3003,
                          "unknown compound assignment operator");
                expr->valueState = ValueState::Unknown;
                return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    // ─── Step 5: Propagate value state ─────────────────────────────────────
    if (expr->op == AssignOp::Assign) {
        expr->valueState = expr->rhs->valueState;
    } else {
        expr->valueState = expr->rhs->valueState;
    }

    return lhsType;
}

// =============================================================================
// resolvePipelineStep
// =============================================================================

TypeAST* resolvePipelineStep(PipelineStepAST* step, const TypeAST* inputType, SemaContext& ctx) {
    if (!step || !inputType) {
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 1: Resolve callable ───────────────────────────────────────────
    TypeAST* callableType = resolveExpr(step->callable, ctx);
    if (!callableType || callableType->isa<UnknownTypeAST>()) {
        ctx.error(step->callable, DiagCode::E2003, "pipeline step callable has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!callableType->isa<FuncTypeAST>()) {
        ctx.error(step->callable, DiagCode::E2003,
                  "pipeline step is not a function type, got ",
                  debug::typeToString(callableType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    if (funcType->params.empty()) {
        ctx.error(step->callable, DiagCode::E3003,
                  "pipeline step function takes no parameters");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Verify first parameter matches input type ────────────────
    const TypeAST* firstParamType = funcType->params[0]->type;
    if (!isAssignable(firstParamType, inputType, ctx)) {
        ctx.error(step->callable, DiagCode::E3003,
                  "pipeline step input mismatch: expected ",
                  debug::typeToString(firstParamType, ctx.pool()),
                  ", got ", debug::typeToString(inputType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Return the output type ────────────────────────────────────
    if (!funcType->returnType) {
        ctx.error(step->callable, DiagCode::E3003,
                  "pipeline step returns void");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    return funcType->returnType;
}

// =============================================================================
// resolvePipelineExpr
// =============================================================================

TypeAST* resolvePipelineExpr(PipelineExprAST* expr, SemaContext& ctx) {
    if (expr->steps.empty()) {
        ctx.error(expr, DiagCode::E1107, "pipeline has no steps");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 1: Resolve seed ──────────────────────────────────────────────
    TypeAST* currentType = resolveExpr(expr->seed, ctx);
    if (!currentType || currentType->isa<UnknownTypeAST>()) {
        ctx.error(expr->seed, DiagCode::E2002, "seed has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Walk through each step ────────────────────────────────────
    for (PipelineStepAST* step : expr->steps) {
        currentType = resolvePipelineStep(step, currentType, ctx);
        if (!currentType || currentType->isa<UnknownTypeAST>()) {
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    return currentType;
}

// =============================================================================
// resolveComposeOperand
// =============================================================================

TypeAST* resolveComposeOperand(ComposeOperandAST* operand, SemaContext& ctx) {
    if (!operand) {
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 1: Resolve callable ──────────────────────────────────────────
    TypeAST* callableType = resolveExpr(operand->callable, ctx);
    if (!callableType || callableType->isa<UnknownTypeAST>()) {
        ctx.error(operand->callable, DiagCode::E2003, "composition operand has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!callableType->isa<FuncTypeAST>()) {
        ctx.error(operand->callable, DiagCode::E2003,
                  "composition operand is not a function type, got ",
                  debug::typeToString(callableType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // ─── Step 2: Verify function has exactly one parameter group ──────────
    if (funcType->isCurried()) {
        ctx.error(operand->callable, DiagCode::E3003,
                  "composition operand must have exactly one parameter group "
                  "(curried functions are not allowed in composition)");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Check generic arguments if present ────────────────────────
    if (!operand->genericArgs.empty()) {
        const FuncDeclAST* funcDecl = resolveCalleeOrError(operand->callable, ctx);
        if (!funcDecl) {
            ctx.error(operand->callable, DiagCode::E2003,
                      "generic arguments applied to non-generic function");
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        for (const TypeAST* arg : operand->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.error(operand->callable, DiagCode::E3002,
                          "invalid generic argument type");
                return ctx.arena().makeType<UnknownTypeAST>();
            }
        }

        if (!validateGenericArguments(operand->genericArgs, funcDecl->genericParams,
                                       operand->callable, ctx)) {
            return ctx.arena().makeType<UnknownTypeAST>();
        }
    }

    return callableType;
}

// =============================================================================
// resolveComposeExpr
// =============================================================================

TypeAST* resolveComposeExpr(ComposeExprAST* expr, SemaContext& ctx) {
    if (expr->operands.empty()) {
        ctx.error(expr, DiagCode::E3003, "composition has no operands");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 1: Resolve left operand ──────────────────────────────────────
    if (!expr->left || !expr->left->isa<ComposeOperandAST>()) {
        ctx.error(expr->left, DiagCode::E2002, "invalid left operand in composition");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    TypeAST* leftType = resolveComposeOperand(expr->left->as<ComposeOperandAST>(), ctx);
    if (!leftType || leftType->isa<UnknownTypeAST>()) {
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!leftType->isa<FuncTypeAST>()) {
        ctx.error(expr->left, DiagCode::E2002, "left operand is not a function");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    FuncTypeAST* currentFunc = leftType->as<FuncTypeAST>();

    // ─── Step 2: Walk through right operands ───────────────────────────────
    for (ComposeOperandAST* operand : expr->operands) {
        TypeAST* operandType = resolveComposeOperand(operand, ctx);
        if (!operandType || operandType->isa<UnknownTypeAST>()) {
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!operandType->isa<FuncTypeAST>()) {
            ctx.error(operand->callable, DiagCode::E2002, "operand is not a function");
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        FuncTypeAST* nextFunc = operandType->as<FuncTypeAST>();

        const TypeAST* prevOutput = currentFunc->returnType;
        const TypeAST* nextInput = nextFunc->params.empty() ? nullptr : nextFunc->params[0]->type;

        if (!prevOutput || !nextInput) {
            ctx.error(operand->callable, DiagCode::E3003,
                      "function input/output mismatch in composition");
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        if (!isAssignable(nextInput, prevOutput, ctx)) {
            ctx.error(operand->callable, DiagCode::E3003,
                      "composition type mismatch: previous output ",
                      debug::typeToString(prevOutput, ctx.pool()),
                      " is not assignable to next input ",
                      debug::typeToString(nextInput, ctx.pool()));
            return ctx.arena().makeType<UnknownTypeAST>();
        }

        currentFunc = nextFunc;
    }

    expr->valueState = ValueState::Definite;
    return currentFunc;
}

// =============================================================================
// resolveAnonFuncExpr
// =============================================================================

TypeAST* resolveAnonFuncExpr(AnonFuncExprAST* expr, SemaContext& ctx) {
    if (!expr->funcType) {
        ctx.error(expr, DiagCode::E2002, "anonymous function has no function type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 1: Resolve the function type ──────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(expr->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Analyze parameters ────────────────────────────────────────
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── Step 3: Analyze body ──────────────────────────────────────────────
    if (!expr->body) {
        ctx.error(expr, DiagCode::E3003, "anonymous function has no body");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    ctx.contexts.pushAnonFunction(expr, funcType, expr->loc);

    bool bodyReturns = false;
    if (expr->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(expr->body->as<BlockStmtAST>(), ctx);
    } else if (expr->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(expr->body->as<ReturnStmtAST>(), ctx);
    } else {
        ctx.error(expr, DiagCode::E3003, "anonymous function has invalid body type");
        ctx.contexts.pop();
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (bodyReturns && !ctx.contexts.returnRequirementsSatisfied()) {
        ctx.error(expr, DiagCode::E3005,
                  "anonymous function has missing nested return");
    }

    ctx.contexts.pop();

    // ─── Step 4: Return the function type ──────────────────────────────────
    if (isNullableType(funcType)) {
        expr->valueState = ValueState::Unknown;
    } else if (isFallibleType(funcType)) {
        expr->valueState = ValueState::Unknown;
    } else {
        expr->valueState = ValueState::Definite;
    }

    return funcType;
}

// =============================================================================
// resolveIfExpr
// =============================================================================

TypeAST* resolveIfExpr(IfExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve condition ──────────────────────────────────────────
    TypeAST* condType = resolveExpr(expr->condition, ctx);
    if (!condType || condType->isa<UnknownTypeAST>()) {
        ctx.error(expr->condition, DiagCode::E3003, "if condition has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (!isBoolType(condType)) {
        ctx.error(expr->condition, DiagCode::E3003,
                  "if condition must be bool, got ",
                  debug::typeToString(condType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    if (isNullableType(condType) || isFallibleType(condType)) {
        ctx.error(expr->condition, DiagCode::E3003,
                  "if condition must be definite (non-nullable, non-fallible)");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Resolve branches ───────────────────────────────────────────
    TypeAST* thenType = resolveExpr(expr->thenBranch, ctx);
    if (!thenType || thenType->isa<UnknownTypeAST>()) {
        ctx.error(expr->thenBranch, DiagCode::E3003, "then branch has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    TypeAST* elseType = resolveExpr(expr->elseBranch, ctx);
    if (!elseType || elseType->isa<UnknownTypeAST>()) {
        ctx.error(expr->elseBranch, DiagCode::E3003, "else branch has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Check branch types are compatible ────────────────────────
    if (!isAssignable(thenType, elseType, ctx)) {
        ctx.error(expr, DiagCode::E3003,
                  "if expression branches have incompatible types: then ",
                  debug::typeToString(thenType, ctx.pool()),
                  ", else ", debug::typeToString(elseType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 4: Propagate value state ──────────────────────────────────────
    if (thenType->isa<UnknownTypeAST>() || elseType->isa<UnknownTypeAST>()) {
        expr->valueState = ValueState::Unknown;
    } else if (isNullableType(thenType) || isNullableType(elseType)) {
        expr->valueState = ValueState::Unknown;
    } else if (isFallibleType(thenType) || isFallibleType(elseType)) {
        expr->valueState = ValueState::Unknown;
    } else {
        expr->valueState = ValueState::Definite;
    }

    return thenType;
}

// =============================================================================
// resolveRangeExpr
// =============================================================================

TypeAST* resolveRangeExpr(RangeExprAST* expr, SemaContext& ctx) {
    // ─── Step 1: Resolve bounds ─────────────────────────────────────────────
    TypeAST* loType = resolveExpr(expr->lo, ctx);
    if (!loType || loType->isa<UnknownTypeAST>()) {
        ctx.error(expr->lo, DiagCode::E3003, "range lower bound has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    TypeAST* hiType = resolveExpr(expr->hi, ctx);
    if (!hiType || hiType->isa<UnknownTypeAST>()) {
        ctx.error(expr->hi, DiagCode::E3003, "range upper bound has unknown type");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 2: Validate bounds are numeric ───────────────────────────────
    if (!isNumericType(loType) || !isNumericType(hiType)) {
        ctx.error(expr, DiagCode::E3003,
                  "range bounds must be numeric, got ",
                  debug::typeToString(loType, ctx.pool()), " and ",
                  debug::typeToString(hiType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 3: Validate bounds are definite ──────────────────────────────
    if (isNullableType(loType) || isFallibleType(loType) ||
        isNullableType(hiType) || isFallibleType(hiType)) {
        ctx.error(expr, DiagCode::E3003,
                  "range bounds must be definite (non-nullable, non-fallible)");
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 4: Validate bounds are same type ─────────────────────────────
    if (!typesEqual(loType, hiType)) {
        ctx.error(expr, DiagCode::E3003,
                  "range bounds must be the same type, got ",
                  debug::typeToString(loType, ctx.pool()), " and ",
                  debug::typeToString(hiType, ctx.pool()));
        return ctx.arena().makeType<UnknownTypeAST>();
    }

    // ─── Step 5: Propagate value state ──────────────────────────────────────
    expr->valueState = ValueState::Definite;

    return loType;
}

} // namespace sema