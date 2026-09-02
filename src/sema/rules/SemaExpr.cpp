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
#include "core/ASTStrings.hpp"
#include "core/builtins/BuiltinTypes.hpp"

#include <unordered_set>
#include <optional>

namespace sema {

// =============================================================================
// resolveExprWithTarget - Main Entry Point
// =============================================================================

TypeAST* resolveExprWithTarget(ExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (!expr) {
        return ctx.getUnknownType();
    }

    if (expr->hasSyntaxError) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
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
        case ASTKind::ArenaAccessExpr:
            result = resolveArenaAccess(expr->as<ArenaAccessExprAST>(), ctx);
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
                                  typeToString(targetType, ctx.pool),
                                  ", got ",
                                  typeToString(result, ctx.pool));
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

TypeAST* resolveLiteralExpr(LiteralExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                result = targetType;
            } else {
                result = ctx.getIntType();
            }
            state = ValueState::Definite;
            break;

        case LiteralKind::Float:
            if (targetType && targetType->isa<PrimitiveTypeAST>() && isFloatType(targetType)) {
                result = targetType;
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
                result = targetType;
                state = ValueState::Nil;
            } else {
                result = ctx.getUnknownType();
                state = ValueState::Nil;
            }
            break;

        case LiteralKind::Err:
            if (targetType && isFallibleType(targetType)) {
                result = targetType;
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

TypeAST* resolveIdentifierExpr(IdentifierExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── Special case: `_` is the discard placeholder ──────────────────────
    if (ctx.pool.lookupView(expr->name) == "_") {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->isConst = false;
        return ctx.getUnknownType();
    }

    // ─── Handle `self` parameter ────────────────────────────────────────────
    // `self` is a special parameter that refers to the current struct instance.
    // It can be synthesized by the parser OR written explicitly by the user.
    if (ctx.pool.lookupView(expr->name) == "self") {
        ValueDeclAST* decl = ctx.lookupValue(expr->name);
        if (!decl || !decl->isa<ParamAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, expr,
                                  "'self' is not available in this context");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            expr->isConst = false;
            return ctx.getUnknownType();
        }

        ParamAST* selfParam = decl->as<ParamAST>();
        expr->resolvedDecl = selfParam;
        expr->resolvedType = selfParam->type;
        expr->valueState = ValueState::Definite;
        expr->isLValue = true;   // self is a reference (l-value)
        expr->isConst = false;   // self is mutable by default
        return selfParam->type;
    }

    // ─── Step 1: Check if this is a generic parameter ─────────────────────
    if (ctx.isGenericParam(expr->name)) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                              "'", ctx.pool.lookup(expr->name), 
                              "' is a generic type parameter, not a value");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->isConst = false;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Look up the value declaration ────────────────────────────
    ValueDeclAST* decl = ctx.lookupValue(expr->name);
    if (!decl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, expr,
                              "undefined value '", ctx.pool.lookup(expr->name), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->isConst = false;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Transform field access through self ──────────────────────
    // If the identifier resolves to a FieldDeclAST, and 'self' is in scope,
    // then this is actually a field access: self.field
    if (decl->isa<FieldDeclAST>()) {
        ValueDeclAST* selfDecl = ctx.lookupValue(ctx.pool.intern("self"));
        if (selfDecl && selfDecl->isa<ParamAST>()) {
            FieldDeclAST* fieldDecl = decl->as<FieldDeclAST>();
            
            // ─── Mark this as an implicit field access ─────────────────────
            expr->isImplicitFieldAccess = true;
            expr->fieldIndex = fieldDecl->fieldIndex;
            
            // ─── Create the self identifier expression ─────────────────────
            IdentifierExprAST* selfIdent = ctx.arena.make<IdentifierExprAST>(
                ctx.pool.intern("self")
            );
            selfIdent->resolvedDecl = selfDecl;
            selfIdent->resolvedType = selfDecl->type;
            selfIdent->loc = expr->loc;
            selfIdent->isLValue = true;
            selfIdent->valueState = ValueState::Definite;
            
            // ─── Store the self object ──────────────────────────────────────
            expr->selfObject = selfIdent;
            expr->resolvedDecl = fieldDecl;
            expr->resolvedType = fieldDecl->type;
            expr->isLValue = true;
            expr->isConst = fieldDecl->isConst();
            expr->valueState = (isNullableType(fieldDecl->type) || 
                                isFallibleType(fieldDecl->type))
                               ? ValueState::Unknown : ValueState::Definite;
            
            return fieldDecl->type;
        }
    }

    // ─── Step 4: Check pending future (async/spawn) ──────────────────────
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

    // ─── Step 5: Closure capture validation ──────────────────────────────
    if (ctx.stack.insideFunction()) {
        bool isInCurrentScope = ctx.isInCurrentScope(expr->name);
        bool isModuleMember = ctx.isModuleMember(expr->name);
        bool isCaptured = !isInCurrentScope && !isModuleMember;
        
        if (isCaptured && isBorrowedType(decl->type)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidCapture, expr,
                                  "closure cannot capture borrowed type '",
                                  ctx.pool.lookup(expr->name),
                                  "' (", typeToString(decl->type, ctx.pool),
                                  ") — closures cannot capture &T or [_]T");
            ctx.diagnostics.note(expr,
                                 "Only owned values can be captured by closures.");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 6: Handle generic arguments ──────────────────────────────────
    if (!expr->genericArgs.empty()) {
        if (!decl->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, expr,
                                  "'", ctx.pool.lookup(expr->name), "' is not a function");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        for (TypeAST* arg : expr->genericArgs) {
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

        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
        
        // Use the function's type (generic args are stored on the expression)
        decl->type = funcDecl->type;
        if (!decl->type) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                                  "'", ctx.pool.lookup(expr->name), "' has no type information");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 7: Determine value state ────────────────────────────────────
    TypeAST* declType = decl->type;
    ValueState state = ValueState::Unknown;
    
    if (decl->isa<EnumVariantAST>() || decl->isa<FuncDeclAST>()) {
        state = ValueState::Definite;
    } else if (decl->isa<VarDeclAST>()) {
        VarDeclAST* var = decl->as<VarDeclAST>();
        state = (var->init && var->init->isConst) ? ValueState::Definite : ValueState::Unknown;
    } else if (decl->isa<ParamAST>()) {
        state = ValueState::Definite;
    } else if (decl->isa<FieldDeclAST>()) {
        state = ValueState::Unknown;
    }

    // ─── Step 8: Set isLValue and isConst ──────────────────────────────────
    if (decl->isa<VarDeclAST>()) {
        VarDeclAST* varDecl = decl->as<VarDeclAST>();
        expr->isLValue = (varDecl->keyword == DeclKeyword::Let);
        expr->isConst = (varDecl->keyword == DeclKeyword::Const) && (state == ValueState::Definite);
    } else if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
        expr->isLValue = (funcDecl->keyword == DeclKeyword::Let);
        expr->isConst = (funcDecl->keyword == DeclKeyword::Const);
    } else if (decl->isa<ParamAST>()) {
        ParamAST* param = decl->as<ParamAST>();
        expr->isLValue = !param->isConstParam;
        expr->isConst = param->isConstParam;
    } else if (decl->isa<EnumVariantAST>()) {
        expr->isLValue = false;
        expr->isConst = true;
    } else if (decl->isa<FieldDeclAST>()) {
        FieldDeclAST* field = decl->as<FieldDeclAST>();
        expr->isLValue = false;  // Set by transform above
        expr->isConst = field->isConst();
    } else {
        expr->isLValue = false;
        expr->isConst = false;
    }

    // ─── Step 9: Apply type narrowing ──────────────────────────────────────
    TypeAST* narrowedType = ctx.stack.getNarrowedType(expr->name);
    if (narrowedType) {
        expr->resolvedType = narrowedType;
        expr->valueState = state;
        expr->isLValue = true;
        return narrowedType;
    }

    // ─── Step 10: Set final type ──────────────────────────────────────────
    expr->resolvedDecl = decl;
    expr->resolvedType = declType;
    expr->valueState = state;
    
    return declType;
}

// =============================================================================
// resolveFieldAccessExpr
// =============================================================================

TypeAST* resolveFieldAccessExpr(FieldAccessExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── Step 1: Resolve object expression ─────────────────────────────
    TypeAST* objectType = resolveExpr(expr->object, ctx);
    if (!objectType || objectType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr->object,
                              "object has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Check if object is nullable or fallible ──────────────
    if (isNullableType(objectType) || isFallibleType(objectType)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, expr->object,
                              "cannot access field on nullable or fallible type '",
                              typeToString(objectType, ctx.pool),
                              "'. Narrow the value first using 'if' or '?\?'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Err;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Handle generic type parameter ────────────────────────
    if (objectType->isa<NamedTypeAST>()) {
        NamedTypeAST* namedType = objectType->as<NamedTypeAST>();

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

            TypeAST* fieldType = getFieldTypeOnGenericType(objectType, expr->fieldName, ctx);
            if (!fieldType) {
                ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                                      "field '", ctx.pool.lookup(expr->fieldName),
                                      "' has no type information in generic constraints");
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }

            expr->resolvedType = fieldType;
            expr->valueState = (isNullableType(fieldType) || isFallibleType(fieldType))
                               ? ValueState::Unknown : ValueState::Definite;
            expr->isLValue = false;
            expr->isConst = false;
            return fieldType;
        }
    }

    // ─── Step 4: Check if object type is a named type ────────────────
    if (!objectType->isa<NamedTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr->object,
                              "field access requires a struct or enum type, got ",
                              typeToString(objectType, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
    
    // ─── Step 5: Resolve the type declaration ──────────────────────────
    TypeDeclAST* typeDecl = namedType->resolvedDecl;
    if (!typeDecl) {
        // Try to look it up if not resolved
        typeDecl = ctx.lookupType(namedType->name);
        if (typeDecl) {
            namedType->resolvedDecl = typeDecl;
        } else {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                                  "undefined type '", ctx.pool.lookup(namedType->name), "'");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    if (typeDecl->hasSyntaxError) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 6: Handle enum type ──────────────────────────────────────
    if (typeDecl->isa<EnumDeclAST>()) {
        EnumDeclAST* enumDecl = typeDecl->as<EnumDeclAST>();
        
        for (size_t i = 0; i < enumDecl->variants.size(); ++i) {
            EnumVariantAST* variant = enumDecl->variants[i];
            if (variant->name == expr->fieldName) {
                if (variant->hasSyntaxError) {
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    expr->isLValue = false;
                    expr->isConst = false;
                    return ctx.getUnknownType();
                }

                expr->resolvedDecl = variant;
                expr->ownerType = enumDecl;
                expr->isEnumAccess = true;
                expr->fieldIndex = i;
                
                expr->resolvedType = ctx.getNamedType(enumDecl->name);
                expr->valueState = ValueState::Definite;
                expr->isLValue = false;
                expr->isConst = true;
                return expr->resolvedType;
            }
        }

        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                              "enum '", ctx.pool.lookup(enumDecl->name),
                              "' has no variant named '", ctx.pool.lookup(expr->fieldName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 7: Handle struct type ────────────────────────────────────
    if (typeDecl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

        // ─── Look up field by name in struct's field list ────────────
        // NOT in the symbol table!
        for (size_t i = 0; i < structDecl->fields.size(); ++i) {
            FieldDeclAST* field = structDecl->fields[i];
            if (field->name == expr->fieldName) {
                if (field->hasSyntaxError) {
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    expr->isLValue = false;
                    expr->isConst = false;
                    return ctx.getUnknownType();
                }

                expr->resolvedDecl = field;
                expr->ownerType = structDecl;
                expr->isEnumAccess = false;
                expr->fieldIndex = i;

                TypeAST* fieldType = field->type;
                if (!fieldType) {
                    fieldType = field->type;
                }
                
                ValueState state = (isNullableType(fieldType) || isFallibleType(fieldType))
                                   ? ValueState::Unknown : ValueState::Definite;
                expr->resolvedType = fieldType;
                expr->valueState = state;
                
                if (expr->object->isLValue) {
                    expr->isLValue = !field->isConst();
                    expr->isConst = field->isConst();
                } else {
                    expr->isLValue = false;
                    expr->isConst = expr->object->isConst;
                }
                return fieldType;
            }
        }

        ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, expr,
                              "struct '", ctx.pool.lookup(structDecl->name),
                              "' has no field named '", ctx.pool.lookup(expr->fieldName), "'");
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

TypeAST* resolveModuleAccessExpr(ModuleAccessExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Look up the member by module alias ─────────────────────────
    ModuleAST* module = ctx.lookupImport(expr->moduleName);
    if (!module) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedModule, expr,
                              "undefined module alias '", ctx.pool.lookup(expr->moduleName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->resolvedDecl = nullptr;
        expr->resolvedModule = nullptr;
        return ctx.getUnknownType();
    }

    ValueDeclAST* decl = ctx.lookupModuleValueMember(module, expr->memberName);
    if (!decl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedMember, expr,
                              "module '", ctx.pool.lookup(expr->moduleName),
                              "' has no member named '", ctx.pool.lookup(expr->memberName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->resolvedDecl = nullptr;
        expr->resolvedModule = module;
        return ctx.getUnknownType();
    }

    if (decl->hasSyntaxError) {
        expr->resolvedDecl = decl;
        expr->resolvedModule = module;
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Store resolved declaration and module ─────────────────────
    expr->resolvedDecl = decl;
    expr->resolvedModule = module;

    // ─── Step 3: Check if the member is exported ───────────────────────────
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

    // ─── Step 4: Get the declaration's type ──────────────────────────────────
    // Use the effective type (accounting for any narrowing that might apply)
    // For module members, narrowing doesn't apply (they're global), but we
    // use the same pattern for consistency.
    TypeAST* declType = ctx.getEffectiveType(decl, expr->memberName);
    if (!declType) {
        // Fallback to decl->type if effective type is not available
        declType = decl->type;
    }
    
    if (!declType) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "member '", ctx.pool.lookup(expr->memberName),
                              "' has no type information");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        return ctx.getUnknownType();
    }

    // ─── Step 5: Set isLValue and isConst based on member's keyword ──────
    if (decl->isa<VarDeclAST>()) {
        VarDeclAST* varDecl = decl->as<VarDeclAST>();
        expr->isLValue = (varDecl->keyword == DeclKeyword::Let);
        expr->isConst = (varDecl->keyword == DeclKeyword::Const);
    } else if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
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

        FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();

        // Resolve each generic argument
        for (TypeAST* arg : expr->genericArgs) {
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

        // Validate generic arguments against the function's parameters
        if (!validateGenericArguments(expr->genericArgs, funcDecl->genericParams, expr, ctx)) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }

        // Use the function's type (generic arguments are stored on the expression)
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
    ValueState state;
    if (decl->isa<EnumVariantAST>()) {
        state = ValueState::Definite;
    } else if (isNullableType(declType) || isFallibleType(declType)) {
        // Nullable/fallible module members are unknown until runtime
        state = ValueState::Unknown;
    } else if (decl->isa<FuncDeclAST>()) {
        // Functions are definite (they exist at compile time)
        state = ValueState::Definite;
    } else if (decl->isa<VarDeclAST>()) {
        VarDeclAST* varDecl = decl->as<VarDeclAST>();
        if (varDecl->init && varDecl->init->isConst) {
            state = ValueState::Definite;
        } else {
            state = ValueState::Unknown;
        }
    } else {
        state = ValueState::Unknown;
    }

    // ─── Step 8: Set the expression's type ──────────────────────────────────
    expr->resolvedType = declType;
    expr->valueState = state;

    // ─── Step 9: Validate against target type if provided ─────────────────
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, declType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "type mismatch: expected ",
                                  typeToString(targetType, ctx.pool),
                                  ", got ",
                                  typeToString(declType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            return ctx.getUnknownType();
        }
    }

    Trace::info("resolveModuleAccessExpr: ", 
             ctx.pool.lookup(expr->moduleName), ":",
             ctx.pool.lookup(expr->memberName),
             " resolved to ", typeToString(declType, ctx.pool));

    return declType;
}

// =============================================================================
// resolveArenaAccess
// =============================================================================

TypeAST* resolveArenaAccess(ArenaAccessExprAST* expr, SemaContext& ctx) {
    if (!expr) return nullptr;
    
    // ─── Step 1: Validate the access using the pure validator ──────────────
    auto methodOpt = builtins::validateArenaAccess(expr, ctx.pool, ctx.diagnostics);
    if (!methodOpt) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        expr->isLValue = false;
        expr->isConst = false;
        return ctx.getUnknownType();
    }
    builtins::ArenaMethodKind method = *methodOpt;
    
    // ─── Step 2: For instance methods, validate the LHS ────────────────────
    if (!expr->isStatic) {
        // Instance form: arena::method()
        
        // ─── 2a: LHS must exist ──────────────────────────────────────────
        if (!expr->arenaExpr) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaInvalidLHS, expr,
                                  "instance arena access requires an Arena expression");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            expr->isConst = false;
            return ctx.getUnknownType();
        }
        
        // ─── 2b: LHS must be Arena type ──────────────────────────────────
        TypeAST* arenaType = expr->arenaExpr->resolvedType;
        if (!arenaType || !isArenaType(arenaType)) {
            ctx.diagnostics.error(DiagCode::Sem_ArenaInvalidLHS, expr,
                                  "arena:: access requires an Arena value, got ",
                                  arenaType ? typeToString(arenaType, ctx.pool) : "unknown");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            expr->isLValue = false;
            expr->isConst = false;
            return ctx.getUnknownType();
        }
        
        // ─── 2c: LHS must be a const binding ──────────────────────────────
        if (expr->arenaExpr->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = expr->arenaExpr->as<IdentifierExprAST>();
            ValueDeclAST* decl = id->resolvedDecl;
            if (decl && decl->isa<VarDeclAST>()) {
                VarDeclAST* varDecl = decl->as<VarDeclAST>();
                if (varDecl->keyword == DeclKeyword::Let) {
                    ctx.diagnostics.error(DiagCode::Sem_ArenaNotConst, expr,
                                          "Arena access requires a const binding");
                    ctx.diagnostics.note(expr,
                                          "Arena bindings must be declared with const");
                    expr->resolvedType = ctx.getUnknownType();
                    expr->valueState = ValueState::Unknown;
                    expr->isLValue = false;
                    expr->isConst = false;
                    return ctx.getUnknownType();
                }
            }
        }
    }
    
    // ─── Step 3: Build the return type ─────────────────────────────────────
    TypeAST* returnType = nullptr;
    TypeAST* genericArg = nullptr;
    
    if (!expr->genericArgs.empty() && expr->genericArgs[0]) {
        genericArg = expr->genericArgs[0];
    }
    
    returnType = builtins::getArenaMethodReturnType(
        method, 
        genericArg,
        ctx.pool,
        ctx.arena
    );
    
    // ─── Step 4: Determine value state based on method ─────────────────────
    ValueState state = ValueState::Definite;
    
    switch (method) {
        case builtins::ArenaMethodKind::Create: {
            // Arena::create(size) -> Arena!
            // Wrap Arena in FallibleTypeAST
            TypeAST* arenaType = ctx.getArenaType();
            returnType = ctx.arena.make<FallibleTypeAST>(arenaType);
            state = ValueState::Err;  // Can fail (out of memory)
            break;
        }
        
        case builtins::ArenaMethodKind::Empty: {
            // Arena::empty() -> Arena
            returnType = ctx.getArenaType();
            state = ValueState::Definite;
            break;
        }
        
        case builtins::ArenaMethodKind::Alloc: {
            // arena::alloc<T>(count) -> [_]T
            // Return type is already ArrayTypeAST with Slice kind
            // The slice is a borrowed view - state is unknown (bounds check at runtime)
            state = ValueState::Unknown;
            break;
        }
        
        case builtins::ArenaMethodKind::Reset: {
            // arena::reset() -> ()
            returnType = nullptr;
            state = ValueState::None;
            break;
        }
        
        case builtins::ArenaMethodKind::Descriptor: {
            // arena::descriptor() -> ArenaDescriptor
            returnType = ctx.getArenaDescriptorType();
            state = ValueState::Definite;
            break;
        }
        
        case builtins::ArenaMethodKind::Capacity:
        case builtins::ArenaMethodKind::Remaining:
        case builtins::ArenaMethodKind::Space: {
            // capacity() -> uint64, remaining() -> uint64, space<T>() -> uint64
            // Return type is already PrimitiveTypeAST (Uint64)
            state = ValueState::Definite;
            break;
        }
        
        case builtins::ArenaMethodKind::IsEmpty:
        case builtins::ArenaMethodKind::CanFit: {
            // isEmpty() -> bool, canFit<T>() -> bool
            // Return type is already PrimitiveTypeAST (Bool)
            state = ValueState::Definite;
            break;
        }
    }
    
    // ─── Step 5: Store results ─────────────────────────────────────────────
    expr->resolvedType = returnType;
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = false;
    
    return returnType;
}

// =============================================================================
// resolveArrayLiteralExpr
// =============================================================================

TypeAST* resolveArrayLiteralExpr(ArrayLiteralExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (expr->elements.empty()) {
        // ─── Empty array: type must be inferred from context ────────────────
        // If targetType is an array type, use its element type
        if (targetType && targetType->isa<ArrayTypeAST>()) {
            ArrayTypeAST* targetArray = targetType->as<ArrayTypeAST>();
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
    TypeAST* targetElemType = nullptr;
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
        ArrayTypeAST* targetArray = targetType->as<ArrayTypeAST>();
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

TypeAST* resolveStructLiteralExpr(StructLiteralExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Look up the struct type ─────────────────────────────────
    TypeDeclAST* typeDecl = ctx.lookupType(expr->typeName);
    if (!typeDecl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "undefined type '", ctx.pool.lookup(expr->typeName), "'");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── ArenaDescriptor cannot be constructed via struct literal ──────
    if (lookupStringView(expr->typeName) == "ArenaDescriptor") {
        ctx.diagnostics.error(DiagCode::Sem_ArenaDescriptorLiteral, expr,
                              "ArenaDescriptor is a built-in type and cannot be constructed "
                              "via struct literal syntax");
        ctx.diagnostics.note(expr,
                              "ArenaDescriptor can only be obtained via arena::descriptor()");
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

    StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    if (structDecl->hasSyntaxError) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

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
            const_cast<TypeAST*&>(expr->genericArgs[i]) = resolvedArg;
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
    std::unordered_map<InternedString, FieldDeclAST*> fieldMap;
    for (FieldDeclAST* field : structDecl->fields) {
        fieldMap[field->name] = field;
    }

    std::unordered_set<InternedString> initializedFields;
    bool hasErr = false;
    bool allDefinite = true;

    // ─── Step 4: Validate each field initializer ─────────────────────────
    for (FieldInitAST* init : expr->inits) {
        auto it = fieldMap.find(init->name);
        if (it == fieldMap.end()) {
            ctx.diagnostics.error(DiagCode::Sem_FieldNotFound, init,
                                  "struct '", ctx.pool.lookup(structDecl->name),
                                  "' has no field named '", ctx.pool.lookup(init->name), "'");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        FieldDeclAST* field = it->second;
        if (field->hasSyntaxError) {
            continue;
        }

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
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->hasSyntaxError) {
            continue;
        }

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

TypeAST* resolveBinaryExpr(BinaryExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                                          typeToString(leftType, ctx.pool), " and ",
                                          typeToString(rightType, ctx.pool));
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
                                  typeToString(targetType, ctx.pool),
                                  ", got ",
                                  typeToString(resultType, ctx.pool));
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

TypeAST* resolveUnaryExpr(UnaryExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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

TypeAST* resolveCallExpr(CallExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
    FuncDeclAST* funcDecl = resolveCalleeOrError(expr->callee, ctx);
    if (funcDecl) {
        if (!expr->genericArgs.empty()) {
            for (TypeAST* arg : expr->genericArgs) {
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
        TypeAST* expectedType = nullptr;
        
        if (hasVariadic && i >= variadicIndex) {
            // ─── This argument goes to the variadic parameter ──────────────────
            // The variadic parameter's type is [*]T (dynamic array)
            // The argument type should be T (element type)
            ParamAST* variadicParam = funcType->params[variadicIndex];
            
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
TypeAST* resolveIntrinsicCallExpr(IntrinsicCallExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
    TypeAST* resultType = getIntrinsicReturnType(expr, targetType, ctx);
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
                                  typeToString(targetType, ctx.pool),
                                  ", got ",
                                  typeToString(resultType, ctx.pool));
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
    expr->resolvedType = resultType;
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = false;

    return resultType;
}

// =============================================================================
// resolveIndexExpr
// =============================================================================

TypeAST* resolveIndexExpr(IndexExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                              typeToString(targetTypeAst, ctx.pool),
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
                              typeToString(targetTypeAst, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    ArrayTypeAST* arrayType = targetTypeAst->as<ArrayTypeAST>();

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

TypeAST* resolveSliceExpr(SliceExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                              typeToString(targetTypeAst, ctx.pool),
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
                              typeToString(targetTypeAst, ctx.pool));
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    ArrayTypeAST* arrayType = targetTypeAst->as<ArrayTypeAST>();

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
// resolveNullCoalesceExpr
// =============================================================================

/// @brief Check if an expression can panic at runtime by inspecting its AST kind.
/// 
/// An expression can panic if it contains:
/// - Division or modulo (could divide by zero)
/// - Array indexing (could be out of bounds)
/// - Slice bounds (could be out of range)
/// - arena::alloc (could be out of capacity)
/// 
/// @note This is purely syntactic - we inspect the AST kind and structure.
///       No flags or metadata are needed.
static bool isPanicProneExpression(ExprAST* expr, SemaContext& ctx) {
    if (!expr) return false;
    
    switch (expr->kind) {
        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            // Division or modulo can divide by zero
            if (bin->op == BinaryOp::Div || bin->op == BinaryOp::Mod) {
                return true;
            }
            // Other ops might contain panic-prone sub-expressions
            return isPanicProneExpression(bin->left, ctx) || 
                   isPanicProneExpression(bin->right, ctx);
        }
        
        case ASTKind::IndexExpr: {
            // Array indexing can be out of bounds
            return true;
        }
        
        case ASTKind::SliceExpr: {
            // Slice bounds can be out of range
            return true;
        }
        
        case ASTKind::ArenaAccessExpr: {
            ArenaAccessExprAST* arena = expr->as<ArenaAccessExprAST>();
            // arena::alloc can fail (out of capacity)
            if (arena->methodName == ctx.pool.intern("alloc")) {
                return true;
            }
            return false;
        }
        
        case ASTKind::CallExpr: {
            // Function calls can panic if the function body can panic
            // We can check if the called function is foreign or contains panic-prone ops
            CallExprAST* call = expr->as<CallExprAST>();
            
            // Check if callee is a foreign function
            if (call->callee && call->callee->isa<IdentifierExprAST>()) {
                IdentifierExprAST* id = call->callee->as<IdentifierExprAST>();
                if (id->resolvedDecl && id->resolvedDecl->isa<FuncDeclAST>()) {
                    FuncDeclAST* func = id->resolvedDecl->as<FuncDeclAST>();
                    if (func->isForeignFunction) {
                        return true;  // Foreign calls can fail
                    }
                    // Check if function is const (const functions can't panic)
                    if (func->isConst()) {
                        return false;
                    }
                    // For regular functions, we'd need to inspect the body
                    // Conservative: assume any function call can panic
                    return true;
                }
            }
            
            // Check arguments for panic-prone expressions
            for (ExprAST* arg : call->args) {
                if (isPanicProneExpression(arg, ctx)) {
                    return true;
                }
            }
            return false;
        }
        
        case ASTKind::UnaryExpr: {
            UnaryExprAST* unary = expr->as<UnaryExprAST>();
            // Unary ops don't panic by themselves
            return isPanicProneExpression(unary->operand, ctx);
        }
        
        case ASTKind::NullCoalesceExpr: {
            // ?? handles panics, so the expression itself is safe
            // But the LHS and RHS might contain panics
            NullCoalesceExprAST* coalesce = expr->as<NullCoalesceExprAST>();
            return isPanicProneExpression(coalesce->value, ctx) ||
                   isPanicProneExpression(coalesce->fallback, ctx);
        }
        
        case ASTKind::PipelineExpr: {
            PipelineExprAST* pipe = expr->as<PipelineExprAST>();
            // Check seed and each step
            if (isPanicProneExpression(pipe->seed, ctx)) return true;
            for (PipelineStepAST* step : pipe->steps) {
                if (isPanicProneExpression(step->callable, ctx)) return true;
                for (ExprAST* arg : step->packArgs) {
                    if (isPanicProneExpression(arg, ctx)) return true;
                }
            }
            return false;
        }
        
        case ASTKind::FieldAccessExpr: {
            FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            // Field access doesn't panic (Sema prevents null/err access)
            // But the object might contain panic-prone expressions
            return isPanicProneExpression(field->object, ctx);
        }
        
        case ASTKind::ModuleAccessExpr: {
            // Module access doesn't panic (members are compile-time known)
            return false;
        }
        
        case ASTKind::IdentifierExpr: {
            // Identifiers don't panic
            return false;
        }
        
        case ASTKind::LiteralExpr: {
            // Literals don't panic
            return false;
        }
        
        default:
            return false;
    }
}

TypeAST* resolveNullCoalesceExpr(NullCoalesceExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (!expr) return ctx.getUnknownType();

    // ─── Step 1: Resolve LHS ────────────────────────────────────────────────
    TypeAST* lhsType = resolveExpr(expr->value, ctx);
    if (!lhsType || lhsType->isa<UnknownTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->value,
                              "LHS has unknown type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Determine if LHS is valid for ?? ──────────────────────────
    bool lhsIsTagged = isNullableType(lhsType) || isFallibleType(lhsType);
    bool lhsCanPanic = isPanicProneExpression(expr->value, ctx);

    if (!lhsIsTagged && !lhsCanPanic) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->value,
                              "?? requires nullable/fallible LHS (T?, T!, or T?!) "
                              "or an expression that can panic (division, indexing, arena::alloc)");
        ctx.diagnostics.note(expr->value,
                             "Use a nullable/fallible value, or ensure the expression can fail");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 3: Determine the inner/result type ────────────────────────────
    TypeAST* innerType = lhsType;
    bool resultIsNullable = false;
    bool resultIsFallible = false;

    if (lhsIsTagged) {
        // ─── Unwrap tagged LHS ──────────────────────────────────────────────
        if (isNullableType(innerType)) {
            innerType = unwrapNullable(innerType);
        }
        if (isFallibleType(innerType)) {
            innerType = unwrapFallible(innerType);
        }
        // ?? removes nullability/fallibility
    } else {
        // ─── Plain panic-prone LHS ──────────────────────────────────────────
        // The result type is the same as LHS (int from 10/d)
        innerType = lhsType;
    }

    // ─── Step 4: Determine the expected RHS type ────────────────────────────
    // RHS must be assignable to innerType
    TypeAST* expectedRhsType = innerType;

    // ─── Step 5: Resolve RHS against expected type ──────────────────────────
    TypeAST* rhsType = resolveExprWithTarget(expr->fallback, expectedRhsType, ctx);
    if (!rhsType || rhsType->isa<UnknownTypeAST>()) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 6: Determine the final result type ────────────────────────────
    TypeAST* resultType = innerType;

    // If target type is provided and is tagged, we may need to wrap the result
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (isNullableType(targetType) || isFallibleType(targetType)) {
            // Target is tagged: result should match target's structure
            TypeAST* targetInner = targetType;
            bool targetIsNullable = false;
            bool targetIsFallible = false;
            
            if (isNullableType(targetInner)) {
                targetInner = unwrapNullable(targetInner);
                targetIsNullable = true;
            }
            if (isFallibleType(targetInner)) {
                targetInner = unwrapFallible(targetInner);
                targetIsFallible = true;
            }
            
            if (typesEqual(innerType, targetInner)) {
                // Result should be wrapped to match target
                TypeAST* wrappedResult = innerType;
                if (targetIsNullable) {
                    wrappedResult = ctx.arena.make<NullableTypeAST>(wrappedResult);
                }
                if (targetIsFallible) {
                    wrappedResult = ctx.arena.make<FallibleTypeAST>(wrappedResult);
                }
                resultType = wrappedResult;
                resultIsNullable = targetIsNullable;
                resultIsFallible = targetIsFallible;
            } else {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                      "?? result type mismatch: expected ",
                                      typeToString(targetType, ctx.pool),
                                      " but inner type is ",
                                      typeToString(innerType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
        } else {
            // Target is non-tagged: result must be assignable
            if (!isAssignable(targetType, innerType, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                      "?? result type mismatch: expected ",
                                      typeToString(targetType, ctx.pool),
                                      ", got ", typeToString(innerType, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
            resultType = targetType;
        }
    } else {
        // No target type: result is the inner type (unwrapped)
        resultType = innerType;
    }

    // ─── Step 7: Propagate value state ──────────────────────────────────────
    ValueState lhsState = expr->value->valueState;
    ValueState rhsState = expr->fallback->valueState;
    
    ValueState state;
    if (lhsIsTagged && (lhsState == ValueState::Nil || lhsState == ValueState::Err)) {
        state = rhsState;
    } else if (lhsState == ValueState::Definite && rhsState == ValueState::Definite) {
        state = ValueState::Definite;
    } else if (lhsState == ValueState::Definite) {
        state = rhsState;
    } else if (rhsState == ValueState::Definite) {
        state = lhsState;
    } else {
        state = ValueState::Unknown;
    }

    expr->resolvedType = resultType;
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = false;
    
    return resultType;
}

// =============================================================================
// resolveAssignExpr
// =============================================================================

TypeAST* resolveAssignExpr(AssignExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                                          typeToString(lhsType, ctx.pool));
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
                                          typeToString(lhsType, ctx.pool));
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
// resolvePipelineStep - Optimized with Caching
// =============================================================================

/// @brief Resolve a single pipeline step with variadic parameter support.
/// 
/// Pipeline steps are always function types (callable). They are never nullable
/// or fallible by definition - a function value itself cannot be nil or err.
/// 
/// Argument order: The upstream values are passed FIRST, then the pack args.
/// 
/// Variadic handling:
///   - The last parameter can be variadic (`...T`), which absorbs all remaining
///     arguments into a slice `[]T`.
///   - The function receives the variadic parameter as a slice.
///   - Extra arguments beyond the function's fixed parameters are absorbed
///     by the variadic parameter.
/// 
/// @param step The pipeline step.
/// @param upstreamType The type of the upstream value (from seed or previous step).
///                     This can be a single value or multiple values packed together.
/// @param ctx The semantic context.
/// @return The return type of the step, or nullptr on error.
TypeAST* resolvePipelineStep(PipelineStepAST* step, TypeAST* upstreamType, SemaContext& ctx) {
    if (!step || !upstreamType) {
        return ctx.getUnknownType();
    }

    // ─── Reject intrinsic calls ────────────────────────────────────────────
    if (step->callable->isa<IntrinsicCallExprAST>()) {
        IntrinsicCallExprAST* intrinsic = step->callable->as<IntrinsicCallExprAST>();
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "intrinsic call cannot be used as a pipeline step");
        ctx.diagnostics.note(step->callable,
                             "Intrinsic '#", ctx.pool.lookup(intrinsic->intrinsicName),
                             "' is a complete call. Use a wrapper function.");
        return ctx.getUnknownType();
    }

    // ─── Step 1: Get the callable type (cached via resolvedType) ────────────
    // The resolvedType is set by resolveExpr and cached on the AST node.
    // This is the single source of truth for this step's callable type.
    TypeAST* callableType = step->callable->resolvedType;
    
    // If not already resolved, resolve it now and cache the result.
    if (!callableType || callableType->isa<UnknownTypeAST>()) {
        callableType = resolveExpr(step->callable, ctx);
        if (!callableType || callableType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, step->callable,
                                  "pipeline step callable has unknown type");
            return ctx.getUnknownType();
        }
        // resolvedType is already set by resolveExpr
    }

    // ─── Step 2: Must be a function type ────────────────────────────────────
    if (!callableType->isa<FuncTypeAST>()) {
        // ─── Check if it's a generic function reference that wasn't instantiated ──
        // Use cached resolvedDecl when available
        ExprAST* callable = step->callable;
        
        if (callable->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = callable->as<IdentifierExprAST>();
            // Use cached resolvedDecl (set by resolveIdentifierExpr)
            ValueDeclAST* decl = id->resolvedDecl;
            if (decl && decl->isa<FuncDeclAST>()) {
                FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
                if (!funcDecl->genericParams.empty() && id->genericArgs.empty()) {
                    ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, step->callable,
                                          "generic function '", ctx.pool.lookup(id->name),
                                          "' requires generic arguments in pipeline step");
                    ctx.diagnostics.note(step->callable,
                                         "Use '", ctx.pool.lookup(id->name), "<T>' to instantiate");
                    return ctx.getUnknownType();
                }
            }
        } else if (callable->isa<ModuleAccessExprAST>()) {
            ModuleAccessExprAST* mod = callable->as<ModuleAccessExprAST>();
            // Use cached resolvedDecl (set by resolveModuleAccessExpr)
            ValueDeclAST* decl = mod->resolvedDecl;
            if (decl && decl->isa<FuncDeclAST>()) {
                FuncDeclAST* funcDecl = decl->as<FuncDeclAST>();
                if (!funcDecl->genericParams.empty() && mod->genericArgs.empty()) {
                    ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, step->callable,
                                          "generic function '", ctx.pool.lookup(mod->memberName),
                                          "' requires generic arguments in pipeline step");
                    ctx.diagnostics.note(step->callable,
                                         "Use '", ctx.pool.lookup(mod->memberName), "<T>' to instantiate");
                    return ctx.getUnknownType();
                }
            }
        } else if (callable->isa<FieldAccessExprAST>()) {
            FieldAccessExprAST* field = callable->as<FieldAccessExprAST>();
            
            // ─── Use cached information if available ───────────────────────────
            if (field->resolvedDecl) {
                if (field->isEnumAccess) {
                    // Enum variant - not a function
                    ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, field,
                                        "enum variant '", ctx.pool.lookup(field->fieldName),
                                        "' is a value, not a function - cannot use in pipeline");
                    ctx.diagnostics.note(field,
                                        "Enum variants are constants. Use a function that returns ",
                                        "the variant if you need it in a pipeline.");
                    return ctx.getUnknownType();
                }
                
                // Struct field - check if it's a function
                // The field's type is already cached in resolvedType
                if (callableType->isa<FuncTypeAST>()) {
                    // Valid: field is a function
                    // No generic args needed - they're already in the type
                    // Continue with normal flow
                } else {
                    ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, field,
                                        "field '", ctx.pool.lookup(field->fieldName),
                                        "' is not a function - cannot use in pipeline");
                    ctx.diagnostics.note(field,
                                        "Only functions can be used in pipelines. The field type is ",
                                        typeToString(callableType, ctx.pool));
                    return ctx.getUnknownType();
                }
            } else {
                // ─── Fallback: resolve the field access ─────────────────────────
                // This will populate the cache
                resolveFieldAccessExpr(field, nullptr, ctx);
                
                // Now check the cached result
                if (field->isEnumAccess) {
                    ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, field,
                                        "enum variant '", ctx.pool.lookup(field->fieldName),
                                        "' is a value, not a function - cannot use in pipeline");
                    return ctx.getUnknownType();
                }
                
                if (!callableType->isa<FuncTypeAST>()) {
                    ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, field,
                                        "field '", ctx.pool.lookup(field->fieldName),
                                        "' is not a function - cannot use in pipeline");
                    ctx.diagnostics.note(field,
                                        "Only functions can be used in pipelines. The field type is ",
                                        typeToString(callableType, ctx.pool));
                    return ctx.getUnknownType();
                }
            }
        }
        
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "pipeline step is not a function type, got ",
                              typeToString(callableType, ctx.pool));
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // ─── Step 3: Validate argument pack usage ──────────────────────────────
    bool isAnonymousFunction = step->callable->isa<AnonFuncExprAST>();
    bool hasPackArgs = !step->packArgs.empty();
    
    if (isAnonymousFunction && hasPackArgs) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "anonymous function cannot have argument pack (!) in pipeline step");
        ctx.diagnostics.note(step->callable,
                             "Anonymous functions capture all arguments at the definition site. "
                             "Use a named function reference if you need argument pack.");
        return ctx.getUnknownType();
    }

    // ─── Step 4: Build the combined argument list ──────────────────────────
    // Order: (upstream_value, pack_args...)
    std::vector<TypeAST*> argTypes;
    
    // ─── 4a: Add the upstream value ──────────────────────────────────────
    // Upstream can be a single value or multiple values packed together.
    if (upstreamType && !upstreamType->isa<UnknownTypeAST>()) {
        argTypes.push_back(upstreamType);
    }
    
    // ─── 4b: Resolve pack arguments ──────────────────────────────────────
    for (ExprAST* packArg : step->packArgs) {
        // packArg->resolvedType is already set by resolveExpr
        TypeAST* argType = packArg->resolvedType;
        if (!argType || argType->isa<UnknownTypeAST>()) {
            argType = resolveExpr(packArg, ctx);
            if (!argType || argType->isa<UnknownTypeAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, packArg,
                                      "pack argument has unknown type");
                return ctx.getUnknownType();
            }
        }
        argTypes.push_back(argType);
    }
    
    // ─── Step 5: Special case: Function takes no parameters ────────────────
    if (funcType->params.empty()) {
        // ─── WARNING: All upstream values are discarded ──────────────────────
        if (!argTypes.empty()) {
            // Build a description of what's being discarded
            std::string discardedTypes;
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (i > 0) discardedTypes += ", ";
                discardedTypes += typeToString(argTypes[i], ctx.pool);
            }
            
            ctx.diagnostics.warning(DiagCode::Warn_DiscardedResult, step->callable,
                                    "pipeline step function takes no parameters, but ",
                                    argTypes.size(), " value(s) are being discarded",
                                    " (", discardedTypes, ")");
            ctx.diagnostics.note(step->callable,
                                 "The function '", typeToString(callableType, ctx.pool),
                                 "' ignores all upstream values. Consider removing this step.");
        }
        
        if (!funcType->returnType) {
            ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                                  "pipeline step returns void (cannot continue pipeline)");
            return ctx.getUnknownType();
        }
        return funcType->returnType;
    }
    
    // ─── Step 6: Check for variadic parameter ──────────────────────────────
    size_t paramCount = funcType->params.size();
    bool hasVariadic = funcType->params.back()->isVariadic;
    size_t fixedParamCount = hasVariadic ? paramCount - 1 : paramCount;
    
    // ─── Step 7: Validate argument count ────────────────────────────────────
    // With variadic:
    //   - Fixed parameters must be satisfied exactly
    //   - Remaining arguments are absorbed by the variadic parameter
    // Without variadic:
    //   - Function can accept FEWER parameters than provided (extra discarded)
    //   - Cannot accept MORE parameters than provided
    size_t argCount = argTypes.size();
    
    if (hasVariadic) {
        // ─── Variadic: Need at least fixedParamCount arguments ──────────────
        // Fixed parameters are required; variadic can be empty or more.
        if (argCount < fixedParamCount) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, step->callable,
                                  "pipeline step expects at least ", fixedParamCount,
                                  " argument(s) (", fixedParamCount, " fixed + variadic), ",
                                  "but only ", argCount, " are available");
            return ctx.getUnknownType();
        }
        // No upper bound - variadic absorbs all remaining
    } else {
        // ─── Non-variadic: Can accept fewer (extra discarded), but not more ──
        if (paramCount > argCount) {
            ctx.diagnostics.error(DiagCode::Sem_ArgCountMismatch, step->callable,
                                  "pipeline step expects ", paramCount,
                                  " argument(s), but only ", argCount, " are available");
            return ctx.getUnknownType();
        }
        
        // ─── WARNING: Extra arguments are being discarded ────────────────────
        if (argCount > paramCount) {
            size_t discardedCount = argCount - paramCount;
            std::string discardedTypes;
            for (size_t i = paramCount; i < argCount; ++i) {
                if (i > paramCount) discardedTypes += ", ";
                discardedTypes += typeToString(argTypes[i], ctx.pool);
            }
            
            ctx.diagnostics.warning(DiagCode::Warn_DiscardedResult, step->callable,
                                    "pipeline step discards ", discardedCount,
                                    " extra argument(s)", 
                                    discardedCount > 0 ? " (" + discardedTypes + ")" : "");
            ctx.diagnostics.note(step->callable,
                                 "The function '", typeToString(callableType, ctx.pool),
                                 "' expects only ", paramCount, " parameter(s), but ",
                                 argCount, " value(s) are available. Extra values are discarded.");
        }
    }
    
    // ─── Step 8: Type-check each parameter ──────────────────────────────────
    for (size_t i = 0; i < paramCount; ++i) {
        TypeAST* paramType = funcType->params[i]->type;
        bool isVariadicParam = hasVariadic && (i == paramCount - 1);
        
        if (isVariadicParam) {
            // ─── Variadic parameter: absorbs all remaining arguments ─────────
            // The parameter type is [*]T (dynamic array) or [N]T (fixed array)
            // The argument type should be T (element type)
            // All remaining arguments must be assignable to T
            
            TypeAST* elementType = nullptr;
            if (paramType->isa<ArrayTypeAST>()) {
                elementType = paramType->as<ArrayTypeAST>()->element;
            } else {
                ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, step->callable,
                                      "variadic parameter must be an array type [*]T or [N]T");
                return ctx.getUnknownType();
            }
            
            // Check all remaining arguments against the element type
            for (size_t j = i; j < argCount; ++j) {
                TypeAST* argType = argTypes[j];
                
                if (!isAssignable(elementType, argType, ctx)) {
                    ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                                          "variadic argument at position ", j + 1,
                                          " type mismatch: expected ",
                                          typeToString(elementType, ctx.pool),
                                          ", got ", typeToString(argType, ctx.pool));
                    return ctx.getUnknownType();
                }
            }
            
            // We're done - all variadic arguments are checked
            break;
            
        } else {
            // ─── Fixed parameter: check the corresponding argument ───────────
            if (i >= argCount) {
                // Should not happen due to count check above, but defensive
                break;
            }
            
            TypeAST* argType = argTypes[i];
            
            // Determine argument source for better diagnostics
            std::string argSource;
            size_t upstreamCount = (upstreamType && !upstreamType->isa<UnknownTypeAST>()) ? 1 : 0;
            if (i < upstreamCount) {
                argSource = " (from upstream)";
            } else {
                size_t packIndex = i - upstreamCount;
                argSource = " (from pack argument " + std::to_string(packIndex + 1) + ")";
            }
            
            if (!isAssignable(paramType, argType, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                                      "pipeline step type mismatch at argument ", i + 1,
                                      argSource, ": expected ",
                                      typeToString(paramType, ctx.pool),
                                      ", got ", typeToString(argType, ctx.pool));
                return ctx.getUnknownType();
            }
        }
    }
    
    // ─── Step 9: Return the function's return type ─────────────────────────
    if (!funcType->returnType) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, step->callable,
                              "pipeline step returns void (cannot continue pipeline)");
        return ctx.getUnknownType();
    }
    
    return funcType->returnType;
}

// =============================================================================
// resolvePipelineExpr - Optimized
// =============================================================================

TypeAST* resolvePipelineExpr(PipelineExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (expr->steps.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_PipelineMismatch, expr,
                              "pipeline has no steps");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve the seed expression ──────────────────────────────
    TypeAST* currentType = expr->seed->resolvedType;
    if (!currentType || currentType->isa<UnknownTypeAST>()) {
        currentType = resolveExpr(expr->seed, ctx);
        if (!currentType || currentType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr->seed,
                                  "pipeline seed has unknown type");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 2: Walk through each step ────────────────────────────────────
    for (PipelineStepAST* step : expr->steps) {
        // resolvePipelineStep uses cached resolvedType from the AST
        TypeAST* stepResult = resolvePipelineStep(step, currentType, ctx);
        
        if (!stepResult || stepResult->isa<UnknownTypeAST>()) {
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
        
        currentType = stepResult;
    }

    // ─── Step 3: Validate against target type if provided ──────────────────
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, currentType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "pipeline result type mismatch: expected ",
                                  typeToString(targetType, ctx.pool),
                                  ", got ", typeToString(currentType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    // ─── Step 4: Propagate value state ─────────────────────────────────────
    ValueState state = ValueState::Definite;
    if (currentType) {
        if (isNullableType(currentType)) {
            state = ValueState::Unknown;
        } else if (isFallibleType(currentType)) {
            state = ValueState::Unknown;
        }
    }

    expr->resolvedType = currentType;
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = false;
    
    return currentType;
}

// =============================================================================
// resolveComposeOperand
// =============================================================================

/// @brief Resolve a composition operand with caching.
/// 
/// Caches the resolved type and declaration information on the
/// operand AST node itself, avoiding double lookup.
/// First call to resolveComposeOperand:
///   ├── operand->callable->resolvedType = nullptr (not resolved yet)
///   ├── call resolveExpr(operand->callable) → sets resolvedType and resolvedDecl
///   ├── cache both on the AST node
///   └── return resolvedType
///
/// Subsequent calls to resolveComposeOperand (same operand):
///   ├── operand->callable->resolvedType != nullptr ✅
///   ├── operand->callable->resolvedDecl != nullptr ✅ (if applicable)
///   └── use cached values directly
/// 
/// @param operand The composition operand.
/// @param targetType The target type (for context-dependent resolution).
/// @param ctx The semantic context.
/// @return The resolved function type, or nullptr on error.
TypeAST* resolveComposeOperand(ComposeOperandAST* operand, TypeAST* targetType, SemaContext& ctx) {
    if (!operand) {
        return ctx.getUnknownType();
    }

    // ─── Step 1: Resolve callable (cached via resolvedType) ────────────────
    // The resolvedType is set by resolveExpr and cached on the AST node.
    // This is the single source of truth for this operand's type.
    TypeAST* callableType = operand->callable->resolvedType;
    
    // If not already resolved, resolve it now and cache the result.
    if (!callableType || callableType->isa<UnknownTypeAST>()) {
        callableType = resolveExpr(operand->callable, ctx);
        if (!callableType || callableType->isa<UnknownTypeAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, operand->callable,
                                  "composition operand has unknown type");
            return ctx.getUnknownType();
        }
        // resolvedType is already set by resolveExpr
    }

    // ─── Step 2: Check if callable is nullable or fallible ─────────────────
    if (isNullableType(callableType) || isFallibleType(callableType)) {
        ctx.diagnostics.error(DiagCode::Sem_IllegalNilErr, operand->callable,
                              "cannot use nullable or fallible value in a composition. "
                              "Narrow first using 'if' or '?\?'.");
        return ctx.getUnknownType();
    }

    // ─── Step 3: Must be a function type ───────────────────────────────────
    if (!callableType->isa<FuncTypeAST>()) {
        // ─── Handle generic function references ──────────────────────────────
        // We need to check if the callable is a generic function reference
        // that requires instantiation. This applies to:
        //   1. IdentifierExprAST   → plain function name
        //   2. ModuleAccessExprAST → module:function
        
        ExprAST* callable = operand->callable;
        bool hasGenericParams = false;
        bool hasGenericArgs = false;
        FuncDeclAST* funcDecl = nullptr;
        
        // ─── Case 1: IdentifierExprAST ─────────────────────────────────────
        if (callable->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = callable->as<IdentifierExprAST>();
            hasGenericArgs = !id->genericArgs.empty();
            
            // Use cached resolvedDecl (set by resolveIdentifierExpr)
            ValueDeclAST* decl = id->resolvedDecl;
            if (decl && decl->isa<FuncDeclAST>()) {
                funcDecl = decl->as<FuncDeclAST>();
                hasGenericParams = !funcDecl->genericParams.empty();
            }
        }
        
        // ─── Case 2: ModuleAccessExprAST ──────────────────────────────────
        else if (callable->isa<ModuleAccessExprAST>()) {
            ModuleAccessExprAST* mod = callable->as<ModuleAccessExprAST>();
            hasGenericArgs = !mod->genericArgs.empty();
            
            // Use cached resolvedDecl (set by resolveModuleAccessExpr)
            ValueDeclAST* decl = mod->resolvedDecl;
            if (decl && decl->isa<FuncDeclAST>()) {
                funcDecl = decl->as<FuncDeclAST>();
                hasGenericParams = !funcDecl->genericParams.empty();
            }
        }
        
        // ─── Case 3: FieldAccessExprAST ────────────────────────────────────
        // Field access in composition:
        //   - If the field type is a function, it can be used in composition
        //   - Generic information comes from the struct literal, NOT from
        //     the field access itself (FieldAccessExprAST has no genericArgs)
        //   - We need to resolve the field's type and check if it's a function
        else if (callable->isa<FieldAccessExprAST>()) {
            FieldAccessExprAST* field = callable->as<FieldAccessExprAST>();
            
            // ─── Check: Is this an enum variant access? ──────────────────
            // Enum variants are values, not functions. They should not be
            // used in composition.
            TypeAST* objectType = field->object->resolvedType;
            if (objectType && objectType->isa<NamedTypeAST>()) {
                NamedTypeAST* namedType = objectType->as<NamedTypeAST>();
                TypeDeclAST* decl = namedType->resolvedDecl;
                if (decl && decl->isa<EnumDeclAST>()) {
                    ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, field,
                                          "enum variant '", ctx.pool.lookup(field->fieldName),
                                          "' is a value, not a function - cannot use in composition");
                    ctx.diagnostics.note(field,
                                         "Enum variants are constants. Use a function that returns ",
                                         "the variant if you need it in a composition chain.");
                    return ctx.getUnknownType();
                }
            }
            
            // ─── Check: Is this a struct field access? ────────────────────
            // The field type must be a function for composition.
            // FieldAccessExprAST itself has no genericArgs - the generic
            // information is already resolved in the field's type from the
            // struct literal that instantiated it.
            if (callableType->isa<FuncTypeAST>()) {
                // The field IS a function - valid for composition
                // No generic args needed here - they're already in the type
                return callableType;
            }
            
            // ─── Field access is not a function ───────────────────────────
            ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, field,
                                  "field '", ctx.pool.lookup(field->fieldName),
                                  "' is not a function - cannot use in composition");
            ctx.diagnostics.note(field,
                                 "Only functions can be composed. The field type is ",
                                 typeToString(callableType, ctx.pool));
            return ctx.getUnknownType();
        }
        
        // ─── Validate generic instantiation ───────────────────────────────
        if (funcDecl) {
            if (hasGenericParams && !hasGenericArgs) {
                ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, operand->callable,
                                      "generic function '", ctx.pool.lookup(funcDecl->name),
                                      "' requires generic arguments in composition");
                ctx.diagnostics.note(operand->callable,
                                     "Use '", ctx.pool.lookup(funcDecl->name), "<T>' to instantiate");
                return ctx.getUnknownType();
            }
            if (!hasGenericParams && hasGenericArgs) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, operand->callable,
                                      "function '", ctx.pool.lookup(funcDecl->name),
                                      "' is not generic but generic arguments were provided");
                return ctx.getUnknownType();
            }
        }
        
        // ─── If we still don't have a function type, error ──────────────
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                              "composition operand is not a function type, got ",
                              typeToString(callableType, ctx.pool));
        return ctx.getUnknownType();
    }

    FuncTypeAST* funcType = callableType->as<FuncTypeAST>();

    // ─── Step 4: Validate generic arguments ────────────────────────────────
    if (!operand->genericArgs.empty()) {
        // Use cached resolvedDecl if available
        FuncDeclAST* funcDecl = nullptr;
        
        if (operand->callable->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = operand->callable->as<IdentifierExprAST>();
            ValueDeclAST* decl = id->resolvedDecl;
            if (decl && decl->isa<FuncDeclAST>()) {
                funcDecl = decl->as<FuncDeclAST>();
            }
        } else if (operand->callable->isa<ModuleAccessExprAST>()) {
            ModuleAccessExprAST* mod = operand->callable->as<ModuleAccessExprAST>();
            ValueDeclAST* decl = mod->resolvedDecl;
            if (decl && decl->isa<FuncDeclAST>()) {
                funcDecl = decl->as<FuncDeclAST>();
            }
        } else {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, operand->callable,
                                  "generic arguments can only be applied to named functions");
            return ctx.getUnknownType();
        }
        
        if (!funcDecl) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, operand->callable,
                                  "generic arguments applied to non-generic function");
            return ctx.getUnknownType();
        }

        for (TypeAST* arg : operand->genericArgs) {
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

    // ─── Step 5: Validate composition-specific rules ──────────────────────
    
    // ─── 5a: No variadic parameters ─────────────────────────────────────
    for (ParamAST* param : funcType->params) {
        if (param->isVariadic) {
            ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                  "variadic parameters are not allowed in composition");
            ctx.diagnostics.note(operand->callable,
                                 "Composition functions must have a single parameter");
            return ctx.getUnknownType();
        }
    }

    // ─── 5b: Exactly one parameter ──────────────────────────────────────
    if (funcType->params.size() != 1) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                              "composition operand must have exactly one parameter, got ",
                              funcType->params.size());
        ctx.diagnostics.note(operand->callable,
                             "Composition chains work with single-parameter functions");
        return ctx.getUnknownType();
    }

    return callableType;
}

// =============================================================================
// resolveComposeExpr - Optimized with Caching
// =============================================================================

TypeAST* resolveComposeExpr(ComposeExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    // ─── Step 1: Validate operands ──────────────────────────────────────────
    if (expr->operands.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, expr,
                              "composition has no operands");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    if (!expr->left || !expr->left->isa<ComposeOperandAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, expr->left,
                              "invalid left operand in composition");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // ─── Step 2: Resolve left operand ──────────────────────────────────────
    // resolveComposeOperand uses cached resolvedType from the AST.
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

    FuncTypeAST* leftFunc = leftType->as<FuncTypeAST>();

    // ─── Step 3: Extract left operand's input and output ───────────────────
    TypeAST* inputType = leftFunc->params[0]->type;
    if (!inputType) {
        ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, expr->left,
                              "left operand has no parameter type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    TypeAST* currentOutput = leftFunc->returnType;

    // ─── Step 4: Process each right operand ─────────────────────────────────
    for (ComposeOperandAST* operand : expr->operands) {
        // resolveComposeOperand uses cached resolvedType from the AST.
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

        FuncTypeAST* rightFunc = operandType->as<FuncTypeAST>();

        TypeAST* rightInput = rightFunc->params[0]->type;
        if (!rightInput) {
            ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                  "operand has no parameter type");
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        TypeAST* rightOutput = rightFunc->returnType;

        // ─── 4c: Validate type compatibility ──────────────────────────────
        bool currentIsVoid = (currentOutput == nullptr);
        bool rightInputIsVoid = (rightInput == nullptr);
        
        if (currentIsVoid && rightInputIsVoid) {
            // Both void - valid
        } else if (!currentIsVoid && !rightInputIsVoid) {
            if (!isAssignable(rightInput, currentOutput, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                      "composition type mismatch: previous output ",
                                      typeToString(currentOutput, ctx.pool),
                                      " is not assignable to next input ",
                                      typeToString(rightInput, ctx.pool));
                expr->resolvedType = ctx.getUnknownType();
                expr->valueState = ValueState::Unknown;
                return ctx.getUnknownType();
            }
        } else {
            if (currentIsVoid && !rightInputIsVoid) {
                ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                      "composition type mismatch: previous output is void, "
                                      "but next operand expects ",
                                      typeToString(rightInput, ctx.pool));
            } else {
                ctx.diagnostics.error(DiagCode::Sem_CompositionMismatch, operand->callable,
                                      "composition type mismatch: previous output ",
                                      typeToString(currentOutput, ctx.pool),
                                      " is not void, but next operand expects void");
            }
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }

        currentOutput = rightOutput;
    }

    // ─── Step 5: Build the composed function type ──────────────────────────
    FuncTypeAST* composedType = ctx.arena.make<FuncTypeAST>();
    composedType->loc = expr->loc;
    
    auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
    paramBuilder.push_back(leftFunc->params[0]);
    composedType->params = paramBuilder.build();
    composedType->returnType = currentOutput;

    // ─── Step 6: Validate against target type ──────────────────────────────
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, composedType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "composed function type mismatch: expected ",
                                  typeToString(targetType, ctx.pool),
                                  ", got ", typeToString(composedType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    expr->resolvedType = composedType;
    expr->valueState = ValueState::Definite;
    expr->isLValue = false;
    expr->isConst = false;
    
    return composedType;
}

// =============================================================================
// resolveAnonFuncExpr - Anonymous function expression (closure)
// =============================================================================

TypeAST* resolveAnonFuncExpr(AnonFuncExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
    if (!expr->funcType) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, expr,
                              "anonymous function has no function type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // 1. Resolve the function type (nested)
    FuncTypeAST* funcType = expr->funcType;
    if (!resolveFuncType(funcType, ctx)) {
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // 2. Store the resolved type
    expr->resolvedType = funcType;

    // ─── 3. Push function scopes using RAII guard ──────────────────────────
    ScopedFunction funcScope(ctx, expr, funcType->returnType);

    // ─── 4. Resolve parameters ─────────────────────────────────────────────
    for (ParamAST* param : funcType->params) {
        resolveParam(param, ctx);
    }

    // 5. Validate body exists
    if (!expr->body) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, expr,
                              "anonymous function has no body");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // 6. Resolve the body
    bool bodyReturns = false;
    if (expr->body->isa<BlockStmtAST>()) {
        bodyReturns = resolveBlock(expr->body->as<BlockStmtAST>(), ctx);
    } else if (expr->body->isa<ReturnStmtAST>()) {
        bodyReturns = resolveReturnStmt(expr->body->as<ReturnStmtAST>(), ctx);
    } else {
        ctx.diagnostics.error(DiagCode::Sem_InvalidUnary, expr,
                              "anonymous function has invalid body type");
        expr->resolvedType = ctx.getUnknownType();
        expr->valueState = ValueState::Unknown;
        return ctx.getUnknownType();
    }

    // 7. Verify return paths
    TypeAST* expectedReturn = funcType->returnType;
    if (expectedReturn && !bodyReturns) {
        ctx.diagnostics.error(DiagCode::Sem_MissingReturn, expr,
                              "anonymous function does not return a value on all paths");
    }

    // 8. Capture analysis
    if (ctx.getClosureDepth() > 0) {
        analyzeCaptures(expr, ctx);
    }

    // ─── 9. ScopedFunction destructor automatically pops scopes ────────────

    // 10. Determine value state
    ValueState state = ValueState::Definite;
    if (expectedReturn) {
        if (isNullableType(expectedReturn)) state = ValueState::Unknown;
        else if (isFallibleType(expectedReturn)) state = ValueState::Err;
        else state = ValueState::Definite;
    } else {
        state = ValueState::None;
    }
    expr->valueState = state;
    expr->isLValue = false;
    expr->isConst = false;

    // 11. Validate against target type if provided
    if (targetType && !targetType->isa<UnknownTypeAST>()) {
        if (!isAssignable(targetType, funcType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, expr,
                                  "anonymous function type mismatch: expected ",
                                  typeToString(targetType, ctx.pool),
                                  ", got ", typeToString(funcType, ctx.pool));
            expr->resolvedType = ctx.getUnknownType();
            expr->valueState = ValueState::Unknown;
            return ctx.getUnknownType();
        }
    }

    return funcType;
}

// =============================================================================
// resolveIfExpr
// =============================================================================

TypeAST* resolveIfExpr(IfExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                              typeToString(thenType, ctx.pool),
                              ", else ", typeToString(elseType, ctx.pool));
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

TypeAST* resolveRangeExpr(RangeExprAST* expr, TypeAST* targetType, SemaContext& ctx) {
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
                              typeToString(loType, ctx.pool), " and ",
                              typeToString(hiType, ctx.pool));
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