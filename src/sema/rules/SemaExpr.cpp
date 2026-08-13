/// @file SemaExpr.cpp
/// @brief Implements Sema.hpp's "EXPRESSIONS - Type Resolution" section.
/// 
/// @design_decision Direct Expression Mutation
///   Each resolver updates the ExprAST node directly (resolvedType, valueState, isLValue, isConst).
///   This leverages the existing infrastructure and avoids duplication.
/// 
/// @design_decision Target Type Validation
///   `resolveExprWithTarget` validates expressions against an expected type.
///   This centralizes type checking and uses cached singleton types.

#include "../Sema.hpp"
#include "../registry/IntrinsicValidator.hpp"
#include "../support/CaptureAnalysis.hpp"

#include <unordered_set>
#include <optional>

namespace sema {

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
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "unsupported expression kind");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
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
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "type mismatch: expected ",
                                  debug::typeToString(targetType, ctx.pool),
                                  ", got ",
                                  debug::typeToString(result, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
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
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "unknown literal kind");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
    }

    expr->resolvedType = result;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    expr->isLValue = false;   // Literals are never l-values
    expr->isConst = true;     // Literals are compile-time constants
    
    return result;
}

// =============================================================================
// resolveIdentifierExpr
// =============================================================================

TypeAST* resolveIdentifierExpr(IdentifierExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Special case: `_` is the discard placeholder ──────────────────────
    if (ctx.pool.lookupView(expr->name) == "_") {
        // `_` has no type - it's a placeholder, not a real value
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->isConst = false;
        return ctx.getUnknownType();
    }

    // ─── Step 1: Check if this is a generic parameter ─────────────────────
    if (ctx.isGenericParam(expr->name)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                              "'", ctx.pool.lookup(expr->name), 
                              "' is a generic type parameter, not a value");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Look up the value declaration ────────────────────────────
    const ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, expr,
                              "undefined value '", ctx.pool.lookup(expr->name), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Get the declaration's type ──────────────────────
    // The type should have been set during resolution of the declaration
    // (resolveVarDecl, resolveParam, resolveFuncDecl, resolveStructFields, etc.)
    const TypeAST* declType = decl->type;
    if (!declType) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "'", ctx.pool.lookup(expr->name), "' has no type information");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 4: Check: Is this a pending future (async/spawn not resolved)? ──
    if (ctx.isPendingFuture(expr->name)) {
        if (ctx.hasPendingAsync(expr->name)) {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, expr,
                                  "cannot use async value '", ctx.pool.lookup(expr->name), 
                                  "'. Use 'await' before using the value.");
        } else if (ctx.hasPendingSpawn(expr->name)) {
            ctx.diagnostics.error(DiagCode::Sem_JoinNonSpawn, expr,
                                  "cannot use spawn value '", ctx.pool.lookup(expr->name), 
                                  "'. Use 'join' before using the value.");
        } else {
            ctx.diagnostics.error(DiagCode::Sem_AwaitNonAsync, expr,
                                  "cannot use future value '", ctx.pool.lookup(expr->name), 
                                  "'. Resolve it first.");
        }
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 5: CLOSURE CAPTURE VALIDATION ──────────────────────────────────
    // If we're inside a function body, check if this variable is captured
    // from an outer scope. Captured variables must not be borrowed types.
    if (ctx.stack.insideFunction()) {
        bool isInCurrentScope = ctx.isInCurrentScope(expr->name);
        bool isModuleMember = ctx.isModuleMember(expr->name);
        
        // A variable is captured if:
        //   1. It's NOT in the current scope (local variable or parameter)
        //   2. It's NOT a module member (top-level declaration)
        bool isCaptured = !isInCurrentScope && !isModuleMember;
        
        if (isCaptured) {
            // ─── Rule 4: No borrowed types in closures ──────────────────────
            if (isBorrowedType(declType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, expr,
                                      "closure cannot capture borrowed type '",
                                      ctx.pool.lookup(expr->name),
                                      "' (", debug::typeToString(declType, ctx.pool),
                                      ") — closures cannot capture &T or [_]T");
                ctx.diagnostics.note(expr,
                                     "Only owned values can be captured by closures. "
                                     "Use a value copy or pass the value as a parameter.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
        }
    }

    // ─── Step 6: Handle generic arguments (function instantiation) ────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "'", ctx.pool.lookup(expr->name), "' is not a function");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        // Resolve each generic argument
        for (const TypePtr arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                      "invalid generic argument type for '",
                                      ctx.pool.lookup(expr->name), "'");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
        }

        // Validate generic arguments against the function's parameters
        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        // ─── For generic function references, use the function's type ──
        // The function's type is the resolved function type.
        // Full generic substitution would go here for instantiated types.
        declType = funcDecl->type;
        if (!declType) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                                  "'", ctx.pool.lookup(expr->name), "' has no type information");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 7: Determine value state ─────────────────────────────────────
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
    } else if (decl->isa<ParamAST>()) {
        state = ValueState::Unknown;
    } else if (decl->isa<FieldDeclAST>()) {
        state = ValueState::Unknown;
    } else {
        state = ValueState::Unknown;
    }

    // ─── Step 8: Set isLValue and isConst based on declaration type ──────
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* varDecl = decl->as<VarDeclAST>();
        expr->isLValue = (varDecl->keyword == DeclKeyword::Let);
        expr->isConst = (varDecl->keyword == DeclKeyword::Const) && (state == ValueState::Definite);
    } else if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        expr->isLValue = (funcDecl->keyword == DeclKeyword::Let);
        expr->isConst = (funcDecl->keyword == DeclKeyword::Const);
    } else if (decl->isa<ParamAST>()) {
        const ParamAST* param = decl->as<ParamAST>();
        expr->isLValue = !param->isConst();
        expr->isConst = param->isConst();
    } else if (decl->isa<EnumVariantAST>()) {
        expr->isLValue = false;
        expr->isConst = true;
    } else if (decl->isa<FieldDeclAST>()) {
        const FieldDeclAST* field = decl->as<FieldDeclAST>();
        expr->isLValue = false;  // Field access sets this based on object mutability
        expr->isConst = field->isConst();
    } else {
        expr->isLValue = false;
        expr->isConst = false;
    }

    // ─── Step 9: Apply type narrowing from if conditions ────────────────────
    const TypeAST* narrowedType = ctx.stack.getNarrowedType(expr->name);
    if (narrowedType) {
        expr->resolvedType = const_cast<TypeAST*>(narrowedType);
        expr->valueState = state;
        expr->isLValue = true;  // Narrowed variables are still l-values
        return const_cast<TypeAST*>(narrowedType);
    }

    // ─── Step 10: Set the expression's type ──────────────────────
    expr->resolvedType = const_cast<TypeAST*>(declType);
    expr->valueState = state;
    
    return const_cast<TypeAST*>(declType);
}

// =============================================================================
// resolveArrayLiteralExpr
// =============================================================================

TypeAST* resolveArrayLiteralExpr(ArrayLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (expr->elements.empty()) {
        // ─── Empty array: type must be inferred from context ────────────────
        // If targetType is an array type, use its element type
        if (targetType && targetType->isa<ArrayTypeAST>()) {
            const ArrayTypeAST* targetArray = targetType->as<ArrayTypeAST>();
            ArrayTypeAST* resultType = ctx.getArrayType(ArrayKind::Dynamic, 0, targetArray->element);
            expr->resolvedType = resultType;
            expr->valueState = ValueState::Definite;
            expr->isLValue = false;
            expr->isConst = true;
            return resultType;
        }
        
        // Otherwise, unknown type
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Definite;
        expr->isLValue = false;
        expr->isConst = true;
        return ctx.getUnknownType();
    }

    // ─── Resolve the first element ──────────────────────────────────────────
    const TypeAST* targetElemType = nullptr;
    if (targetType && targetType->isa<ArrayTypeAST>()) {
        targetElemType = targetType->as<ArrayTypeAST>()->element;
    }

    TypeAST* firstType = resolveExprWithTarget(expr->elements[0], targetElemType, ctx);
    if (!firstType || firstType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr,
                              "array literal element has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Check all elements against the first type ──────────────────────────
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
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr,
                              "array literal contains elements of different types");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Propagate value state ──────────────────────────────────────────────
    ValueState state;
    if (hasErr) {
        state = ValueState::Err;
    } else if (allDefinite) {
        state = ValueState::Definite;
    } else {
        state = ValueState::Unknown;
    }

    // ─── Use cached array type ──────────────────────────────────────────────
    // If targetType is a fixed array, use its size
    ArrayKind kind = ArrayKind::Dynamic;
    uint64_t size = 0;
    if (targetType && targetType->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* targetArray = targetType->as<ArrayTypeAST>();
        kind = targetArray->arrayKind;
        size = targetArray->size;
    }

    ArrayTypeAST* arrayType = ctx.getArrayType(kind, size, firstType);
    expr->resolvedType = arrayType;
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = allDefinite;
    
    return arrayType;
}

// =============================================================================
// resolveStructLiteralExpr
// =============================================================================

TypeAST* resolveStructLiteralExpr(StructLiteralExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Look up the struct type ─────────────────────────────────
    const TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "undefined type '", ctx.pool.lookup(expr->typeName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    if (!typeDecl->isa<StructDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "'", ctx.pool.lookup(expr->typeName), "' is not a struct");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // ─── Step 2: Check and validate generic arguments ────────────────────
    if (!expr->genericArgs.empty()) {
        // ─── 2a. Check arity ─────────────────────────────────────────────
        if (expr->genericArgs.size() != structDecl->genericParams.size()) {
            ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, expr,
                                  "struct '", ctx.pool.lookup(structDecl->name),
                                  "' expected ", structDecl->genericParams.size(),
                                  " generic arguments, got ",
                                  expr->genericArgs.size());
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        // ─── 2b. Resolve each generic argument type ──────────────────────
        for (size_t i = 0; i < expr->genericArgs.size(); ++i) {
            TypeAST* resolvedArg = resolveType(expr->genericArgs[i], ctx);
            if (!resolvedArg) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                      "invalid generic argument at position ", i + 1,
                                      " for struct '", ctx.pool.lookup(structDecl->name), "'");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
            const_cast<TypePtr&>(expr->genericArgs[i]) = resolvedArg;
        }

        // ─── 2c. Validate constraints ─────────────────────────────────────
        if (!validateGenericArguments(expr->genericArgs, structDecl->genericParams, expr, ctx)) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    } else if (!structDecl->genericParams.empty()) {
        // ─── 2d. Struct has generic parameters but no arguments provided ──
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, expr,
                              "struct '", ctx.pool.lookup(structDecl->name),
                              "' requires ", structDecl->genericParams.size(),
                              " generic argument(s)");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
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
            ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, init,
                                  "struct '", ctx.pool.lookup(structDecl->name),
                                  "' has no field named '", ctx.pool.lookup(init->name), "'");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        const FieldDeclAST* field = it->second;

        // ─── 4a. Const field validation ─────────────────────────────────
        if (field->isConst()) {
            if (init->value->isa<LiteralExprAST>()) {
                const LiteralExprAST* literal = init->value->as<LiteralExprAST>();
                if (literal->kind == LiteralKind::Nil || literal->kind == LiteralKind::Err) {
                    ctx.diagnostics.error(DiagCode::Sem_ConstNullable, init,
                                          "const field '", ctx.pool.lookup(field->name),
                                          "' cannot be assigned '",
                                          (literal->kind == LiteralKind::Nil ? "nil" : "err"),
                                          "' (const fields must have definite values)");
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    return ctx.getUnknownType();
                }
            }
        }

        // ─── 4b. Check if field has a block body (function field) ────────
        // If the field has a defaultBody, the literal can provide a value
        // But the value can be a function reference OR an anonymous function
        // (since we're at a struct literal site, anonymous functions are allowed)
        bool isFunctionType = field->type && field->type->isa<FuncTypeAST>();

        // ─── 4c. Resolve initializer against the field type ─────────────
        TypeAST* initType = resolveExprWithTarget(init->value, field->type, ctx);
        if (!initType || initType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        // ─── 4d. Special validation for function fields ──────────────────
        if (isFunctionType && field->defaultBody) {
            // The field has a block body at declaration site.
            // At struct literal site, the user can override it with:
            //   - A function reference (existingFn)
            //   - An anonymous function (func_literal)
            //   - Any expression that evaluates to a function value
            
            // Check if the initializer is a function value
            if (!isFunctionValue(init->value, ctx)) {
                // But wait - if the initializer is a block (which would be
                // an anonymous function at struct literal site), it should
                // be allowed because we're at a struct literal site.
                // The parser would have parsed it as an AnonFuncExpr,
                // which is a function value.
                
                // If it's not a function value, report an error
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, init,
                                      "field '", ctx.pool.lookup(field->name),
                                      "' must be initialized with a function value");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
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

        // ─── 5a. Check if field has a default value or default body ──────
        // If the field has a defaultVal or defaultBody, it's optional
        if (field->defaultVal || field->defaultBody) {
            continue;
        }

        // ─── 5b. Nullable and fallible fields are optional ──────────────
        if (isNullableType(field->type)) {
            continue;
        }

        if (isFallibleType(field->type)) {
            continue;
        }

        // ─── 5c. Combined (T?!) fields must be explicitly initialized ──
        if (field->type->isa<CombinedTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                                  "combined field '", ctx.pool.lookup(field->name),
                                  "' (T?!) must be explicitly initialized (no implicit default)");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        // ─── 5d. Function fields with no default are required ────────────
        if (field->type->isa<FuncTypeAST>()) {
            // Function fields without a defaultBody must be initialized
            // (defaultVal would be a function reference at declaration site)
            // If neither is present, the field is required
            ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                                  "function field '", ctx.pool.lookup(field->name),
                                  "' must be initialized in struct literal (no default body)");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        // ─── 5e. Plain fields with no default are required ──────────────
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, expr,
                              "field '", ctx.pool.lookup(field->name),
                              "' must be initialized in struct literal (no default value)");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
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
    expr->resolvedType = resultType;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    expr->isLValue = false;   // Struct literals are never l-values
    expr->isConst = allDefinite;  // All fields must be const for the struct to be const
    
    return resultType;
}

// =============================================================================
// resolveBinaryExpr
// =============================================================================

TypeAST* resolveBinaryExpr(BinaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve operands ──────────────────────────────────────────
    TypeAST* leftType = resolveExpr(expr->left, ctx);
    if (!leftType || leftType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr->left,
                              "left operand has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    TypeAST* rightType = resolveExpr(expr->right, ctx);
    if (!rightType || rightType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr->right,
                              "right operand has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    ValueState leftState = expr->left->valueState;
    ValueState rightState = expr->right->valueState;

    // ─── Step 2: Check if we're in an if condition context ────────────────
    if (ctx.stack.isIfConditionCtx()) {
        NarrowingInfo info = detectNarrowingPattern(expr, ctx);
        if (info.hasNarrowing) {
            ctx.stack.setPendingNarrowing(info);
            expr->resolvedType = ctx.getBoolType();
            expr->valueState = ValueState::Definite;
            expr->isLValue = false;
            expr->isConst = false;
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
            // ─── Reject nullable/fallible operands outright ────────────────
            // Checked against the declared type (not just flow state) so an
            // un-narrowed nullable/fallible variable is caught even when it
            // hasn't been observed as literal nil/err yet.
            if (isNullableType(leftType) || isFallibleType(leftType) ||
                isNullableType(rightType) || isFallibleType(rightType) ||
                leftState == ValueState::Nil || rightState == ValueState::Nil ||
                leftState == ValueState::Err || rightState == ValueState::Err) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                                      "arithmetic operator cannot be used with a nullable or "
                                      "fallible operand. Narrow first using 'if' or '?\?'.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Err;
                return ctx.getUnknownType();
            }

            if (!isNumericType(leftType) || !isNumericType(rightType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr,
                                      "arithmetic operator requires numeric operands");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            // ─── Numeric promotion rules ────────────────────────────────────
            // 1. If either operand is float → result is float
            // 2. Both integers → promote to larger type
            if (isFloatType(leftType) || isFloatType(rightType)) {
                // int → float promotion
                resultType = ctx.getFloatType();
            } else {
                // Both integers → promote to larger type
                if (!typesEqual(leftType, rightType)) {
                    resultType = getLargerIntegerType(leftType, rightType, ctx);
                } else {
                    resultType = leftType;
                }
            }

            resultState = ValueState::Definite;
            break;
        }

        // ─── Comparison Operators ──────────────────────────────────────────
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge: {
            // ─── Numeric comparisons: allow mixed types ──────────────────
            if (isNumericType(leftType) && isNumericType(rightType)) {
                resultType = ctx.getBoolType();
                resultState = ValueState::Definite;
                break;
            }
            
            // ─── Non-numeric comparisons: must be same type ──────────────
            if (!typesEqual(leftType, rightType)) {
                // Allow nil/err comparisons with nullable/fallible types
                if (!(isNullableType(leftType) || isFallibleType(leftType) ||
                      isNullableType(rightType) || isFallibleType(rightType))) {
                    ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                          "comparison of incompatible types: ",
                                          debug::typeToString(leftType, ctx.pool), " and ",
                                          debug::typeToString(rightType, ctx.pool));
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    return ctx.getUnknownType();
                }
            }
            
            resultType = ctx.getBoolType();
            resultState = ValueState::Definite;
            break;
        }

        // ─── Logical Operators ─────────────────────────────────────────────
        case BinaryOp::And:
        case BinaryOp::Or: {
            if (isNullableType(leftType) || isFallibleType(leftType) ||
                isNullableType(rightType) || isFallibleType(rightType) ||
                leftState == ValueState::Nil || rightState == ValueState::Nil ||
                leftState == ValueState::Err || rightState == ValueState::Err) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                                      "logical operator cannot be used with a nullable or "
                                      "fallible operand. Narrow first using 'if' or '?\?'.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Err;
                return ctx.getUnknownType();
            }

            if (!isBoolType(leftType) || !isBoolType(rightType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidLogicalOp, expr,
                                      "logical operator requires bool operands");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
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
            if (isNullableType(leftType) || isFallibleType(leftType) ||
                isNullableType(rightType) || isFallibleType(rightType) ||
                leftState == ValueState::Nil || rightState == ValueState::Nil ||
                leftState == ValueState::Err || rightState == ValueState::Err) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                                      "bitwise operator cannot be used with a nullable or "
                                      "fallible operand. Narrow first using 'if' or '?\?'.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Err;
                return ctx.getUnknownType();
            }

            if (!isIntegerType(leftType) || !isIntegerType(rightType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidBitwiseOp, expr,
                                      "bitwise operator requires integer operands");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            // Bitwise operators: promote to larger type
            if (!typesEqual(leftType, rightType)) {
                resultType = getLargerIntegerType(leftType, rightType, ctx);
            } else {
                resultType = leftType;
            }

            resultState = ValueState::Definite;
            break;
        }

        default:
            ctx.diagnostics.error(DiagCode::Sem_InvalidBinary, expr,
                                  "unknown binary operator");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
    }

    // Validate against target type if provided
    if (targetType && resultType && !resultType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, resultType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "type mismatch: expected ",
                                  debug::typeToString(targetType, ctx.pool),
                                  ", got ",
                                  debug::typeToString(resultType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    expr->resolvedType = resultType;
    expr->valueState = resultState;
    expr->isLValue = false;
    expr->isConst = false;
    
    return resultType;
}

// =============================================================================
// resolveUnaryExpr
// =============================================================================

TypeAST* resolveUnaryExpr(UnaryExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    TypeAST* operandType = resolveExpr(expr->operand, ctx);
    if (!operandType || operandType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr->operand,
                              "operand has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    ValueState operandState = expr->operand->valueState;
    bool isNullableOrFallible = isNullableType(operandType) || isFallibleType(operandType) ||
                                 operandState == ValueState::Nil || operandState == ValueState::Err;

    TypeAST* resultType = nullptr;
    ValueState resultState = ValueState::Definite;

    switch (expr->op) {
        case UnaryOp::Neg: {
            if (isNullableOrFallible) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                                      "negation cannot be used with a nullable or fallible "
                                      "operand. Narrow first using 'if' or '?\?'.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Err;
                return ctx.getUnknownType();
            }

            if (!isNumericType(operandType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                      "negation requires numeric operand");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            resultType = operandType;
            resultState = ValueState::Definite;
            break;
        }

        case UnaryOp::Not: {
            if (isNullableOrFallible) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                                      "logical not cannot be used with a nullable or fallible "
                                      "operand. Narrow first using 'if' or '?\?'.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Err;
                return ctx.getUnknownType();
            }

            if (!isBoolType(operandType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                      "logical not requires bool operand");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            resultType = ctx.getBoolType();
            resultState = ValueState::Definite;
            break;
        }

        case UnaryOp::BitNot: {
            if (isNullableOrFallible) {
                ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                                      "bitwise not cannot be used with a nullable or fallible "
                                      "operand. Narrow first using 'if' or '?\?'.");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Err;
                return ctx.getUnknownType();
            }

            if (!isIntegerType(operandType)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                      "bitwise not requires integer operand");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            resultType = operandType;
            resultState = ValueState::Definite;
            break;
        }

        default:
            ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                                  "unknown unary operator");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
    }

    expr->resolvedType = resultType;
    expr->valueState = resultState;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    expr->isLValue = false;   // Unary expressions are never l-values
    expr->isConst = false;    // Unary expressions are not compile-time constants
    
    return resultType;
}

// =============================================================================
// resolveCallExpr
// =============================================================================

TypeAST* resolveCallExpr(CallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve callee ─────────────────────────────────────────────
    TypeAST* calleeType = resolveExpr(expr->callee, ctx);
    if (!calleeType || calleeType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, expr->callee,
                              "callee has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if callee is nullable or fallible ────────────────────
    if (isNullableType(calleeType) || isFallibleType(calleeType)) {
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, expr->callee,
                              "cannot call nullable or fallible value. Narrow first using 'if' or '?\?'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Err;
        return ctx.getUnknownType();
    }

    if (!calleeType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, expr->callee,
                              "expression is not callable");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = calleeType->as<FuncTypeAST>();

    // ─── Step 3: Check generic arguments ────────────────────────────────────
    const FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);
    if (funcDecl) {
        if (!expr->genericArgs.empty()) {
            for (const TypeAST* arg : expr->genericArgs) {
                if (!resolveType(arg, ctx)) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                          "invalid generic argument type for '",
                                          ctx.pool.lookup(funcDecl->name), "'");
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    return ctx.getUnknownType();
                }
            }

            if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
        } else if (!funcDecl->genericParams.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, expr,
                                  "generic function '", ctx.pool.lookup(funcDecl->name),
                                  "' requires ", funcDecl->genericParams.size(),
                                  " generic argument(s)");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    } else {
        if (!expr->genericArgs.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "generic arguments can only be applied to named function calls");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 4: Check argument count with variadic support ─────────────────
    size_t requiredArgs = 0;
    size_t totalArgs = funcType->params.size();
    bool hasVariadic = false;
    size_t variadicIndex = totalArgs;
    
    // Find the variadic parameter (if any)
    for (size_t i = 0; i < totalArgs; ++i) {
        if (funcType->params[i]->isVariadic) {
            hasVariadic = true;
            variadicIndex = i;
            break;
        }
    }

    if (hasVariadic) {
        // ─── Variadic function ─────────────────────────────────────────────────
        // Required arguments are all parameters before the variadic
        requiredArgs = variadicIndex;
        
        if (expr->args.size() < requiredArgs) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "function expects at least ", requiredArgs,
                                  " argument(s), got ", expr->args.size(),
                                  " (variadic parameter starts at position ", 
                                  variadicIndex + 1, ")");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
        // No upper bound check - variadic can take unlimited arguments
    } else {
        // ─── Non-variadic function ─────────────────────────────────────────────
        if (expr->args.size() != totalArgs) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, expr,
                                  "wrong number of arguments: expected ", totalArgs,
                                  ", got ", expr->args.size());
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 5: Check each argument type ──────────────────────────────────
    bool hasErrArg = false;
    
    for (size_t i = 0; i < expr->args.size(); ++i) {
        ExprAST* arg = expr->args[i];
        
        // Determine the expected parameter type
        const TypeAST* expectedType = nullptr;
        
        if (hasVariadic && i >= variadicIndex) {
            // ─── This argument goes to the variadic parameter ──────────────────
            // The variadic parameter's type is [*]T (dynamic array)
            // The argument type should be T (element type)
            const ParamAST* variadicParam = funcType->params[variadicIndex];
            
            // The type should be [*]T
            if (variadicParam->type->isa<ArrayTypeAST>()) {
                expectedType = variadicParam->type->as<ArrayTypeAST>()->element;
            } else {
                // Fallback - should not happen
                ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, expr,
                                      "variadic parameter has invalid type (expected array)");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
        } else {
            // ─── Regular parameter ──────────────────────────────────────────────
            expectedType = funcType->params[i]->type;
        }

        // Resolve the argument against the expected type
        TypeAST* argType = resolveExprWithTarget(arg, expectedType, ctx);
        if (!argType || argType->isa<UnknownTypeAST>()) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        if (arg->valueState == ValueState::Err) {
            hasErrArg = true;
        }

        if (arg->valueState == ValueState::Err && !isFallibleType(expectedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                                  "cannot pass err to non-fallible parameter");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        if (arg->valueState == ValueState::Nil && !isNullableType(expectedType)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, arg,
                                  "cannot pass nil to non-nullable parameter");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 6: Propagate value state ──────────────────────────────────────
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

    expr->resolvedType = funcType->returnType;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    expr->isLValue = false;   // Function calls are never l-values
    expr->isConst = false;    // Function calls are not compile-time constants
    
    return funcType->returnType;
}

// =============================================================================
// resolveIntrinsicCallExpr
// =============================================================================

/// NOTE: this is the entry point where register callbacks for #scope_exit intrinsic
///
/// 1.resolveIntrinsicCallExpr calls validateIntrinsicCall
/// 2.validateIntrinsicCall dispatches to validateScopeExit for #scope_exit
/// 3.validateScopeExit validates the call AND registers it on the current block
/// 4.validateIntrinsicCall returns true
/// 5.resolveIntrinsicCallExpr continues with normal void intrinsic handling
TypeAST* resolveIntrinsicCallExpr(IntrinsicCallExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Validate the intrinsic call ──────────────────────────────────
    // This will handle all validation AND registration for scope_exit
    if (!validateIntrinsicCall(expr, ctx)) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if this intrinsic returns void ──────────────────────────
    // For scope_exit, validateIntrinsicCall already registered it and marked it as void.
    // We just need to check if it's void and return accordingly.
    if (isIntrinsicVoid(expr->intrinsicName, ctx)) {
        // Void intrinsics are only valid as statements
        if (targetType != nullptr) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "intrinsic '#", ctx.pool.lookup(expr->intrinsicName),
                                  "' returns no value and cannot be used in an assignment or expression context");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
        
        expr->resolvedType = nullptr;
        expr->valueState = ValueState::None;
        expr->isLValue = false;
        expr->isConst = false;
        return nullptr;
    }

    // ─── Step 3: Get the return type for non-void intrinsics ───────────────────
    const TypeAST* resultType = getIntrinsicReturnType(expr, targetType, ctx);
    if (!resultType) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "intrinsic '#", ctx.pool.lookup(expr->intrinsicName),
                              "' unexpectedly returns no value");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    if (resultType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 4: Validate return type against target type ────────────────────
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, resultType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "type mismatch: expected ",
                                  debug::typeToString(targetType, ctx.pool),
                                  ", got ",
                                  debug::typeToString(resultType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 5: Get value state ──────────────────────────────────────────────
    ValueState state = getIntrinsicValueState(expr, ctx);

    // ─── Step 6: Store LLVM intrinsic ID if available ────────────────────────
    IntrinsicRegistry& registry = IntrinsicRegistry::getInstance(ctx.pool);
    const IntrinsicInfo* info = registry.getInfo(expr->intrinsicName);
    if (info && info->isValid()) {
        expr->intrinsicID = info->llvmID;
    }

    // ─── Step 7: Store results ──────────────────────────────────────────────────
    expr->resolvedType = const_cast<TypeAST*>(resultType);
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = false;

    return const_cast<TypeAST*>(resultType);
}

// =============================================================================
// resolveIndexExpr
// =============================================================================

TypeAST* resolveIndexExpr(IndexExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve target ─────────────────────────────────────────────
    TypeAST* targetTypeAst = resolveExpr(expr->target, ctx);
    if (!targetTypeAst || targetTypeAst->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr->target,
                              "index target has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if target is nullable or fallible ────────────────────
    if (isNullableType(targetTypeAst) || isFallibleType(targetTypeAst)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr->target,
                              "cannot index nullable or fallible value '",
                              debug::typeToString(targetTypeAst, ctx.pool),
                              "'. Narrow the value first using 'if' or '?\?'");
        ctx.diagnostics.note(expr->target,
                             "Use 'if x != nil' or 'if x != err' to narrow, or 'x ?? default'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Err;
        return ctx.getUnknownType();
    }

    if (!targetTypeAst->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr->target,
                              "indexing requires an array target type, got ",
                              debug::typeToString(targetTypeAst, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    const ArrayTypeAST* arrayType = targetTypeAst->as<ArrayTypeAST>();

    // ─── Step 3: Resolve index against int type ─────────────────────────────
    PrimitiveTypeAST* intType = ctx.getIntType();
    TypeAST* indexType = resolveExprWithTarget(expr->index, intType, ctx);
    if (!indexType || indexType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 4: Propagate value state ──────────────────────────────────────
    ValueState state;
    if (isNullableType(arrayType->element) || isFallibleType(arrayType->element)) {
        state = ValueState::Unknown;
    } else {
        state = ValueState::Definite;
    }

    expr->resolvedType = arrayType->element;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Array indexing is an l-value iff the target array is an l-value
    // (you can assign to nums[1] if nums is let)
    expr->isLValue = expr->target->isLValue;
    expr->isConst = expr->target->isConst;
    
    return arrayType->element;
}

// =============================================================================
// resolveSliceExpr
// =============================================================================

TypeAST* resolveSliceExpr(SliceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve target ─────────────────────────────────────────────
    TypeAST* targetTypeAst = resolveExpr(expr->target, ctx);
    if (!targetTypeAst || targetTypeAst->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr->target,
                              "slice target has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if target is nullable or fallible ────────────────────
    if (isNullableType(targetTypeAst) || isFallibleType(targetTypeAst)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr->target,
                              "cannot slice nullable or fallible value '",
                              debug::typeToString(targetTypeAst, ctx.pool),
                              "'. Narrow the value first using 'if' or '?\?'");
        ctx.diagnostics.note(expr->target,
                             "Use 'if x != nil' or 'if x != err' to narrow, or 'x ?? default'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Err;
        return ctx.getUnknownType();
    }

    if (!targetTypeAst->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, expr->target,
                              "slicing requires an array target type, got ",
                              debug::typeToString(targetTypeAst, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    const ArrayTypeAST* arrayType = targetTypeAst->as<ArrayTypeAST>();

    // ─── Step 3: Resolve start bound against int type ──────────────────────
    if (expr->start) {
        PrimitiveTypeAST* intType = ctx.getIntType();
        TypeAST* startType = resolveExprWithTarget(expr->start, intType, ctx);
        if (!startType || startType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 4: Resolve end bound against int type ────────────────────────
    if (expr->end) {
        PrimitiveTypeAST* intType = ctx.getIntType();
        TypeAST* endType = resolveExprWithTarget(expr->end, intType, ctx);
        if (!endType || endType->isa<UnknownTypeAST>()) {
            // Error already reported by resolveExprWithTarget
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 5: Propagate value state ──────────────────────────────────────
    ValueState state;
    if (isNullableType(arrayType->element) || isFallibleType(arrayType->element)) {
        state = ValueState::Unknown;
    } else {
        state = ValueState::Definite;
    }

    // Result is always a slice (cached)
    ArrayTypeAST* sliceType = ctx.getArrayType(ArrayKind::Slice, 0, arrayType->element);
    expr->resolvedType = sliceType;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Slices are never l-values (you can't assign to a slice expression)
    expr->isLValue = false;
    expr->isConst = false;
    
    return sliceType;
}

// =============================================================================
// resolveFieldAccessExpr
// =============================================================================

TypeAST* resolveFieldAccessExpr(FieldAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve object ─────────────────────────────────────────────
    TypeAST* objectType = resolveExpr(expr->object, ctx);
    if (!objectType || objectType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr->object,
                              "object has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if object is nullable or fallible ────────────────────
    if (isNullableType(objectType) || isFallibleType(objectType)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr->object,
                              "cannot access field on nullable or fallible type '",
                              debug::typeToString(objectType, ctx.pool),
                              "'. Narrow the value first using 'if' or '?\?'");
        ctx.diagnostics.note(expr->object,
                             "Use 'if x != nil' or 'if x != err' to narrow, or 'x ?? default'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Err;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Handle generic type parameter ─────────────────────────────
    if (objectType->isa<NamedTypeAST>()) {
        const NamedTypeAST* namedType = objectType->as<NamedTypeAST>();

        if (ctx.isGenericParam(namedType->name)) {
            if (!isFieldAccessibleOnGenericType(objectType, expr->fieldName, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                                      "field '", ctx.pool.lookup(expr->fieldName),
                                      "' is not accessible on generic type '",
                                      ctx.pool.lookup(namedType->name),
                                      "' (no trait constraint provides this field)");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            const TypeAST* fieldType = getFieldTypeOnGenericType(objectType, expr->fieldName, ctx);
            if (!fieldType) {
                ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                                      "field '", ctx.pool.lookup(expr->fieldName),
                                      "' has no type information in generic constraints");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            ValueState state = (isNullableType(fieldType) || isFallibleType(fieldType))
                               ? ValueState::Unknown : ValueState::Definite;
            expr->resolvedType = const_cast<TypeAST*>(fieldType);
            expr->valueState = state;
            expr->isLValue = false;
            expr->isConst = false;
            return const_cast<TypeAST*>(fieldType);
        }
    }

    // ─── Step 4: Look up type declaration ──────────────────────────────────
    if (!objectType->isa<NamedTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr->object,
                              "field access requires a struct or enum type, got ",
                              debug::typeToString(objectType, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    const NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
    const TypeDeclAST* typeDecl = ctx.lookupType(namedType->name);
    if (!typeDecl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "undefined type '", ctx.pool.lookup(namedType->name), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 5: Handle struct type ─────────────────────────────────────────
    if (typeDecl->isa<StructDeclAST>()) {
        const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

        for (const FieldDeclAST* f : structDecl->fields) {
            if (f->name == expr->fieldName) {
                // ─── Get field type from resolvedType (resolved) ────────────
                const TypeAST* fieldType = f->type;
                if (!fieldType) {
                    // Fallback to parser type if resolvedType not set
                    fieldType = f->type;
                }
                
                // ─── Propagate value state ──────────────────────────────────
                ValueState state = (isNullableType(fieldType) || isFallibleType(fieldType))
                                   ? ValueState::Unknown : ValueState::Definite;
                expr->resolvedType = const_cast<TypeAST*>(fieldType);
                expr->valueState = state;
                
                // ─── Set isLValue and isConst ───────────────────────────────
                if (expr->object->isLValue) {
                    if (f->isConst()) {
                        expr->isLValue = false;
                        expr->isConst = true;
                    } else {
                        expr->isLValue = true;
                        expr->isConst = expr->object->isConst;
                    }
                } else {
                    expr->isLValue = false;
                    expr->isConst = expr->object->isConst;
                }
                
                return const_cast<TypeAST*>(fieldType);
            }
        }

        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                              "struct '", ctx.pool.lookup(structDecl->name),
                              "' has no field named '", ctx.pool.lookup(expr->fieldName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 6: Handle enum type ───────────────────────────────────────────
    if (typeDecl->isa<EnumDeclAST>()) {
        const EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();

        for (const EnumVariantAST* v : enumDecl->variants) {
            if (v->name == expr->fieldName) {
                expr->resolvedType = ctx.getNamedType(enumDecl->name);
                expr->valueState = ValueState::Definite;
                expr->isLValue = false;
                expr->isConst = true;
                return ctx.getNamedType(enumDecl->name);
            }
        }

        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                              "enum '", ctx.pool.lookup(enumDecl->name),
                              "' has no variant named '", ctx.pool.lookup(expr->fieldName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                          "field access on unsupported type");
    expr->resolvedType = ctx.getUnknownType();
    expr->valueState = ValueState::Unknown;
    return ctx.getUnknownType();
}

// =============================================================================
// resolveModuleAccessExpr
// =============================================================================

TypeAST* resolveModuleAccessExpr(ModuleAccessExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Look up the member by module alias ─────────────────────────
    const ValueDeclAST* decl = ctx.lookupValueByAlias(expr->moduleName, expr->memberName);
    if (!decl) {
        // The helper already reported the error (module not found or member not found)
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if the member is exported ───────────────────────────
    if (!ctx.isValueExported(decl)) {
        ctx.diagnostics.error(DiagCode::Sem_PrivateMember, expr,
                              "member '", ctx.pool.lookup(expr->memberName),
                              "' in module '", ctx.pool.lookup(expr->moduleName),
                              "' is not exported");
        ctx.diagnostics.note(expr, "Add @[export] to the member declaration to make it accessible");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Mark as module member ────────────────────────────────────
    expr->isModuleMember = true;

    // ─── Step 4: Get the declaration's type ──────────────────────
    const TypeAST* declType = decl->type;
    if (!declType) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "member '", ctx.pool.lookup(expr->memberName),
                              "' has no type information");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 5: Set isLValue based on member's keyword ──────────────────
    if (decl->isa<VarDeclAST>()) {
        const VarDeclAST* varDecl = decl->as<VarDeclAST>();
        expr->isLValue = (varDecl->keyword == DeclKeyword::Let);
        expr->isConst = (varDecl->keyword == DeclKeyword::Const);
    } else if (decl->isa<FuncDeclAST>()) {
        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        expr->isLValue = (funcDecl->keyword == DeclKeyword::Let);
        expr->isConst = (funcDecl->keyword == DeclKeyword::Const);
    } else if (decl->isa<EnumVariantAST>()) {
        expr->isLValue = false;
        expr->isConst = true;
    } else {
        expr->isLValue = false;
        expr->isConst = false;
    }

    // ─── Step 6: Handle generic arguments if present ────────────────────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "member '", ctx.pool.lookup(expr->memberName),
                                  "' is not a generic function");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        const FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        for (const TypePtr arg : expr->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                      "invalid generic argument type for '",
                                      ctx.pool.lookup(expr->memberName), "'");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                expr->isLValue = false;
                return ctx.getUnknownType();
            }
        }

        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        // Use the function's type
        declType = funcDecl->type;
        if (!declType) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                                  "member '", ctx.pool.lookup(expr->memberName),
                                  "' has no type information");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 7: Determine value state ──────────────────────────────────────
    ValueState state = (isNullableType(declType) || isFallibleType(declType))
                       ? ValueState::Unknown : ValueState::Definite;
    if (decl->isa<EnumVariantAST>()) {
        state = ValueState::Definite;
    }

    // ─── Step 8: Set the expression's type ────────────────────────
    expr->resolvedType = const_cast<TypeAST*>(declType);
    expr->valueState = state;
    return const_cast<TypeAST*>(declType);
}

// =============================================================================
// resolveNullCoalesceExpr
// =============================================================================

TypeAST* resolveNullCoalesceExpr(NullCoalesceExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve LHS ────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->value, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->value,
                              "LHS has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    if (!isNullableType(lhsType) && !isFallibleType(lhsType)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->value,
                              "?? requires nullable or fallible LHS (T?, T!, or T?!)");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
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
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr,
                              "cannot unwrap LHS type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Resolve RHS against LHS inner type ────────────────────────
    TypeAST* rhsType = resolveExprWithTarget(expr->fallback, lhsInner, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
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

    expr->resolvedType = rhsType;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Null coalesce expressions are never l-values
    expr->isLValue = false;
    expr->isConst = false;
    
    return rhsType;
}

// =============================================================================
// resolveAssignExpr
// =============================================================================

TypeAST* resolveAssignExpr(AssignExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve LHS ─────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->lhs, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, expr->lhs,
                              "LHS has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if LHS is an l-value ─────────────────────────────────
    if (!expr->lhs->isLValue) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, expr->lhs,
                              "cannot assign to non-l-value expression");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Check if LHS is const ──────────────────────────────────────
    if (expr->lhs->isConst) {
        ctx.diagnostics.error(DiagCode::Sem_ConstAssignment, expr->lhs,
                              "cannot assign to const expression");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 4: Resolve RHS against LHS type ──────────────────────────────
    TypeAST* rhsType = resolveExprWithTarget(expr->rhs, lhsType, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        // Error already reported by resolveExprWithTarget
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 5: Compound assignment operator validation ───────────────────
    if (expr->op != AssignOp::Assign) {
        // Compound assignment (+=, -=, &=, etc.) performs an operation on the
        // current value, so — unlike plain '=' — the LHS must not be
        // nullable or fallible; there's nothing to narrow it against a
        // second time inline.
        if (isNullableType(lhsType) || isFallibleType(lhsType)) {
            ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr->lhs,
                                  "compound assignment cannot be used on a nullable or "
                                  "fallible value. Narrow first using 'if' or '?\?'.");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Err;
            return ctx.getUnknownType();
        }

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
                    ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, expr,
                                          "arithmetic compound assignment requires numeric type, got ",
                                          debug::typeToString(lhsType, ctx.pool));
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    return ctx.getUnknownType();
                }
                break;

            case AssignOp::BitAndAssign:
            case AssignOp::BitOrAssign:
            case AssignOp::BitXorAssign:
            case AssignOp::ShlAssign:
            case AssignOp::ShrAssign:
                if (!isIntegerType(lhsType)) {
                    ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, expr,
                                          "bitwise compound assignment requires integer type, got ",
                                          debug::typeToString(lhsType, ctx.pool));
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    return ctx.getUnknownType();
                }
                break;

            default:
                ctx.diagnostics.error(DiagCode::Sem_InvalidAssignment, expr,
                                      "unknown compound assignment operator");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
        }
    }

    expr->resolvedType = lhsType;
    expr->valueState = expr->rhs->valueState;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Assignment expressions are never l-values (the result is a value)
    expr->isLValue = false;
    expr->isConst = false;
    
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
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, step->callable,
                              "pipeline step callable has unknown type");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if callable is nullable or fallible ──────────────────
    if (isNullableType(callableType) || isFallibleType(callableType)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, step->callable,
                              "cannot use nullable or fallible value as a pipeline step. "
                              "Narrow first using 'if' or '?\?'.");
        return ctx.getUnknownType();
    }

    if (!callableType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "pipeline step is not a function type, got ",
                              debug::typeToString(callableType, ctx.pool));
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    if (funcType->params.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "pipeline step function takes no parameters");
        return ctx.getUnknownType();
    }

    // ─── Step 2: Verify first parameter matches input type ────────────────
    const TypeAST* firstParamType = funcType->params[0]->type;
    if (!isAssignable(firstParamType, inputType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "pipeline step input mismatch: expected ",
                              debug::typeToString(firstParamType, ctx.pool),
                              ", got ", debug::typeToString(inputType, ctx.pool));
        return ctx.getUnknownType();
    }

    // ─── Step 3: Return the output type ────────────────────────────────────
    if (!funcType->returnType) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
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
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, expr,
                              "pipeline has no steps");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve seed ──────────────────────────────────────────────
    TypeAST* currentType = resolveExpr(expr->seed, ctx);
    if (!currentType || currentType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->seed,
                              "seed has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Walk through each step ────────────────────────────────────
    for (PipelineStepAST* step : expr->steps) {
        currentType = resolvePipelineStep(step, currentType, ctx);
        if (!currentType || currentType->isa<UnknownTypeAST>()) {
            return ctx.getUnknownType();
        }
    }

    expr->resolvedType = currentType;
    expr->valueState = ValueState::Definite;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Pipeline expressions are never l-values
    expr->isLValue = false;
    expr->isConst = false;
    
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
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, operand->callable,
                              "composition operand has unknown type");
        return ctx.getUnknownType();
    }

    // ─── Step 1b: Check if callable is nullable or fallible ─────────────────
    if (isNullableType(callableType) || isFallibleType(callableType)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, operand->callable,
                              "cannot use nullable or fallible value in a composition. "
                              "Narrow first using 'if' or '?\?'.");
        return ctx.getUnknownType();
    }

    if (!callableType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                              "composition operand is not a function type, got ",
                              debug::typeToString(callableType, ctx.pool));
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // ─── Step 2: Verify function has exactly one parameter group ──────────
    if (funcType->isCurried()) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                              "composition operand must have exactly one parameter group "
                              "(curried functions are not allowed in composition)");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Check generic arguments if present ────────────────────────
    if (!operand->genericArgs.empty()) {
        const FuncDeclAST* funcDecl = resolveCalleeOrError(operand->callable, ctx);
        if (!funcDecl) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, operand->callable,
                                  "generic arguments applied to non-generic function");
            return ctx.getUnknownType();
        }

        for (const TypeAST* arg : operand->genericArgs) {
            if (!resolveType(arg, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, operand->callable,
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
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, expr,
                              "composition has no operands");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve left operand ──────────────────────────────────────
    if (!expr->left || !expr->left->isa<ComposeOperandAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, expr->left,
                              "invalid left operand in composition");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    TypeAST* leftType = resolveComposeOperand(expr->left->as<ComposeOperandAST>(), nullptr, ctx);
    if (!leftType || leftType->isa<UnknownTypeAST>()) {
        return ctx.getUnknownType();
    }

    if (!leftType->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, expr->left,
                              "left operand is not a function");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
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
            ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                  "operand is not a function");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        FuncTypeAST* nextFunc = operandType->as<FuncTypeAST>();

        const TypeAST* prevOutput = currentFunc->returnType;
        const TypeAST* nextInput = nextFunc->params.empty() ? nullptr : nextFunc->params[0]->type;

        if (!prevOutput || !nextInput) {
            ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                  "function input/output mismatch in composition");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        if (!isAssignable(nextInput, prevOutput, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                  "composition type mismatch: previous output ",
                                  debug::typeToString(prevOutput, ctx.pool),
                                  " is not assignable to next input ",
                                  debug::typeToString(nextInput, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        currentFunc = nextFunc;
    }

    expr->resolvedType = currentFunc;
    expr->valueState = ValueState::Definite;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Composition expressions are never l-values
    expr->isLValue = false;
    expr->isConst = false;
    
    return currentFunc;
}

// =============================================================================
// resolveAnonFuncExpr - Anonymous function expression (closure)
// =============================================================================

TypeAST* resolveAnonFuncExpr(AnonFuncExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    if (!expr->funcType) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "anonymous function has no function type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve the function type ──────────────────────────────────
    FuncTypeAST* funcType = const_cast<FuncTypeAST*>(expr->funcType);
    if (!resolveFuncType(funcType, ctx)) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Store the resolved function type ──────────────────────────
    expr->resolvedType = funcType;

    // ─── Step 3: Push scope for parameters and analyze body ────────────────
    ctx.pushScope();

    // ─── Step 4: Register parameters in the new scope ──────────────────────
    for (FuncTypeAST* group = funcType; group; group = group->getNext()) {
        for (ParamAST* param : group->params) {
            resolveParam(param, ctx);
        }
    }

    // ─── Step 5: Analyze the body ──────────────────────────────────────────
    if (!expr->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, expr,
                              "anonymous function has no body");
        ctx.popScope();
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 6: Push function context for return validation ──────────────
    const TypeAST* expectedReturn = funcType->returnType;
    ctx.stack.pushAnonFunction(expr, expectedReturn);

    bool bodyReturns = false;

    // ─── Step 7: Resolve the body based on its kind ────────────────────────
    if (expr->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(expr->body->as<BlockStmtAST>(), ctx);
    } else if (expr->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(expr->body->as<ReturnStmtAST>(), ctx);
    } else {
        ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                              "anonymous function has invalid body type");
        ctx.stack.pop();
        ctx.popScope();
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 8: Verify return paths ───────────────────────────────────────
    if (expectedReturn && !bodyReturns) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, expr,
                              "anonymous function does not return a value on all paths");
    }

    // ─── Step 9: Pop the function context ──────────────────────────────────
    ctx.stack.pop();

    // ─── Step 10: Detect captures and store them ────────────────────────────
    // The context stack still has the function frame (for the anonymous
    // function), so getClosureDepth() returns the correct depth.
    // We pass the expression to analyzeCaptures which will detect if the
    // anonymous function captures any variables from outer scopes.
    analyzeCaptures(expr, ctx);

    // ─── Step 11: Pop the parameter scope ──────────────────────────────────
    ctx.popScope();

    // ─── Step 12: Set value state ──────────────────────────────────────────
    ValueState state = ValueState::Definite;
    if (expectedReturn) {
        if (isNullableType(expectedReturn)) {
            state = ValueState::Unknown;
        } else if (isFallibleType(expectedReturn)) {
            state = ValueState::Err;
        } else {
            state = ValueState::Definite;
        }
    } else {
        state = ValueState::None;
    }

    expr->valueState = state;
    
    // ─── Step 13: Set isLValue and isConst ──────────────────────────────────
    expr->isLValue = false;
    
    // A closure is const only if it captures no mutable state and all its
    // operations are const. This requires further analysis.
    // For now, we conservatively mark it as not const.
    expr->isConst = false;

    // ─── Step 14: Validate against target type if provided ─────────────────
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, funcType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "anonymous function type mismatch: expected ",
                                  debug::typeToString(targetType, ctx.pool),
                                  ", got ",
                                  debug::typeToString(funcType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    LOG_SEMA("resolveAnonFuncExpr: resolved anonymous function at depth ",
             ctx.getClosureDepth(), " with ",
             expr->captures.size(), " captures",
             expr->hasClosure ? " (closure)" : " (no closure)");

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
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->thenBranch,
                              "then branch has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    TypeAST* elseType = resolveExpr(expr->elseBranch, ctx);
    if (!elseType || elseType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->elseBranch,
                              "else branch has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Check branch types are compatible ────────────────────────
    if (!isAssignable(thenType, elseType, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "if expression branches have incompatible types: then ",
                              debug::typeToString(thenType, ctx.pool),
                              ", else ", debug::typeToString(elseType, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
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

    expr->resolvedType = thenType;
    expr->valueState = state;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // If expressions are never l-values
    expr->isLValue = false;
    expr->isConst = false;
    
    return thenType;
}

// =============================================================================
// resolveRangeExpr
// =============================================================================

TypeAST* resolveRangeExpr(RangeExprAST* expr, const TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Resolve lower bound ────────────────────────────────────────
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
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                              "range bounds must be the same type, got ",
                              debug::typeToString(loType, ctx.pool), " and ",
                              debug::typeToString(hiType, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    expr->resolvedType = loType;
    expr->valueState = ValueState::Definite;
    
    // ─── Set isLValue ──────────────────────────────────────────────────────
    // Range expressions are never l-values
    expr->isLValue = false;
    expr->isConst = false;
    
    return loType;
}

} // namespace sema