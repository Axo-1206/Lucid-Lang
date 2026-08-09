/// @file SemaResolve.cpp
/// @brief Implementation of type resolution functions.

#include "SemaResolve.hpp"
#include "SemaCompare.hpp"
#include "SemaValidate.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace sema {

// ─── Main Resolution Entry Point ─────────────────────────────────────────

TypeAST* resolveType(const TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType: 
            return resolvePrimitiveType(type->as<PrimitiveTypeAST>(), ctx);
        case ASTKind::NamedType:     
            return resolveNamedType(type->as<NamedTypeAST>(), ctx);
        case ASTKind::ModuleTypeAccess:
            return resolveModuleTypeAccess(type->as<ModuleTypeAccessAST>(), ctx);
        case ASTKind::ArrayType:     
            return resolveArrayType(type->as<ArrayTypeAST>(), ctx);
        case ASTKind::NullableType:  
            return resolveNullableType(type->as<NullableTypeAST>(), ctx);
        case ASTKind::FallibleType:  
            return resolveFallibleType(type->as<FallibleTypeAST>(), ctx);
        case ASTKind::CombinedType:  
            return resolveCombinedType(type->as<CombinedTypeAST>(), ctx);
        case ASTKind::RefType:       
            return resolveRefType(type->as<RefTypeAST>(), ctx);
        case ASTKind::PtrType:       
            return resolvePtrType(type->as<PtrTypeAST>(), ctx);
        case ASTKind::FuncType:      
            return resolveFuncType(type->as<FuncTypeAST>(), ctx);
        default:
            ctx.diagnostics.error(DiagCode::Sem_UnknownType, type,
                                  "unknown type");
            return nullptr;
    }
}

// ─── Primitive Type ──────────────────────────────────────────────────────

TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx) {
    (void)ctx;
    return const_cast<PrimitiveTypeAST*>(type);
}

// ─── Named Type ──────────────────────────────────────────────────────────

TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── 1. Check: Is this a generic parameter? ──────────────────────────
    if (ctx.isGenericParam(type->name)) {
        return const_cast<NamedTypeAST*>(type);
    }

    // ─── 2. Look up as concrete type ──────────────────────────────────────
    const TypeDeclAST* decl = ctx.lookupType(type->name);
    if (!decl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, type,
                              "undefined type '", ctx.pool.lookup(type->name), "'");
        return nullptr;
    }

    // ─── 3. Resolve generic arguments if present ─────────────────────────
    if (!type->genericArgs.empty()) {
        if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (type->genericArgs.size() != traitDecl->genericParams.size()) {
                ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                                      "trait '", ctx.pool.lookup(type->name),
                                      "' expected ", traitDecl->genericParams.size(),
                                      " generic arguments, got ", 
                                      type->genericArgs.size());
                return nullptr;
            }
        } else if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (type->genericArgs.size() != structDecl->genericParams.size()) {
                ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                                      "struct '", ctx.pool.lookup(type->name),
                                      "' expected ", structDecl->genericParams.size(),
                                      " generic arguments, got ", 
                                      type->genericArgs.size());
                return nullptr;
            }
        } else if (decl->isa<EnumDeclAST>()) {
            // Enums cannot be generic - but we should validate
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "enum '", ctx.pool.lookup(type->name), "' is not generic");
            return nullptr;
        }

        // Resolve each generic argument type
        for (const TypePtr arg : type->genericArgs) {
            if (!resolveType(arg, ctx)) {
                return nullptr;
            }
        }

        // Validate constraints
        if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (!validateGenericArguments(type->genericArgs, structDecl->genericParams, type, ctx)) {
                return nullptr;
            }
        } else if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (!validateGenericArguments(type->genericArgs, traitDecl->genericParams, type, ctx)) {
                return nullptr;
            }
        }
    }

    return const_cast<NamedTypeAST*>(type);
}

// ─── Module Type Access ──────────────────────────────────────────────────

TypeAST* resolveModuleTypeAccess(const ModuleTypeAccessAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── Step 1: Look up the type in the module by alias ──────────────────
    // Using the new simplified helper
    const TypeDeclAST* decl = ctx.lookupTypeByAlias(type->moduleName, type->typeName);
    if (!decl) {
        // The helper already reported the error (module not found or type not found)
        return nullptr;
    }

    // ─── Step 2: Check if the type is exported ─────────────────────────────
    if (!ctx.isTypeExported(decl)) {
        ctx.diagnostics.error(DiagCode::Sem_PrivateMember, type,
                              "type '", ctx.pool.lookup(type->typeName), "' in module '",
                              ctx.pool.lookup(type->moduleName), "' is not exported");
        ctx.diagnostics.note(type, "Add @[export] to the type declaration to make it accessible");
        return nullptr;
    }

    // ─── Step 3: Validate generic arguments if present ─────────────────────
    if (!type->genericArgs.empty()) {
        // Check if the type is generic
        size_t expectedParams = 0;
        bool isGeneric = false;

        if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            expectedParams = structDecl->genericParams.size();
            isGeneric = true;
        } else if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            expectedParams = traitDecl->genericParams.size();
            isGeneric = true;
        }

        if (!isGeneric) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "type '", ctx.pool.lookup(type->typeName), "' is not generic");
            return nullptr;
        }

        // Check arity
        if (type->genericArgs.size() != expectedParams) {
            ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                                  "type '", ctx.pool.lookup(type->typeName),
                                  "' expected ", expectedParams,
                                  " generic arguments, got ", type->genericArgs.size());
            return nullptr;
        }

        // Resolve each generic argument type
        for (const TypePtr arg : type->genericArgs) {
            if (!resolveType(arg, ctx)) {
                return nullptr;
            }
        }

        // Validate constraints
        if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (!validateGenericArguments(type->genericArgs, structDecl->genericParams, type, ctx)) {
                return nullptr;
            }
        } else if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (!validateGenericArguments(type->genericArgs, traitDecl->genericParams, type, ctx)) {
                return nullptr;
            }
        }
    } else {
        // Check if the type requires generic arguments
        bool requiresGeneric = false;
        if (decl->isa<StructDeclAST>()) {
            const StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (!structDecl->genericParams.empty()) {
                requiresGeneric = true;
            }
        } else if (decl->isa<TraitDeclAST>()) {
            const TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (!traitDecl->genericParams.empty()) {
                requiresGeneric = true;
            }
        }

        if (requiresGeneric) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, type,
                                  "type '", ctx.pool.lookup(type->typeName),
                                  "' requires generic arguments");
            ctx.diagnostics.note(type, "Use '", ctx.pool.lookup(type->moduleName), ":",
                                 ctx.pool.lookup(type->typeName), "<...>' to provide them");
            return nullptr;
        }
    }

    // ─── Step 4: Return the resolved type ──────────────────────────────────
    // Create a NamedTypeAST that represents the resolved type
    NamedTypeAST* resolvedType = ctx.getNamedType(type->typeName);
    resolvedType->genericArgs = type->genericArgs;
    resolvedType->loc = type->loc;

    return resolvedType;
}

// ─── Array Type ──────────────────────────────────────────────────────────

TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* element = resolveType(type->element, ctx);
    if (!element) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidArrayElement, type,
                              "invalid array element type");
        return nullptr;
    }

    // ─── Check: Array element cannot be a reference type ──────────────────
    if (element->isa<RefTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_RefInArray, type,
                              "reference type (&T) cannot be stored in an array");
        return nullptr;
    }

    // ─── Check: Array element cannot be a slice type ──────────────────────
    // Rule 2: No Array/Slice Storage - an array cannot store [_]T as element
    if (element->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* innerArray = element->as<ArrayTypeAST>();
        if (innerArray->isSlice()) {
            ctx.diagnostics.error(DiagCode::Sem_RefInArray, type,
                                  "slice type ([_]T) cannot be stored in an array element");
            return nullptr;
        }
    }

    // ─── Apply Downward Flow Rule to slices ──────────────────────────────
    // A slice ([_]T) is a borrowed type, so it must follow the Downward Flow Rule
    // This is checked in validateBorrowedContext, but we also need to check
    // if the array itself is a slice being used in an invalid context
    if (type->isSlice()) {
        // Check if the slice is being used as a function return, struct field, etc.
        if (!validateBorrowedContext(type, ctx)) {
            return nullptr;
        }
    }

    return const_cast<ArrayTypeAST*>(type);
}

// ─── Nullable Type ──────────────────────────────────────────────────────

TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, type,
                              "invalid nullable inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FunctionNullable, type,
                              "function types cannot be nullable");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be nullable (use empty array instead)");
        return nullptr;
    }

    return const_cast<NullableTypeAST*>(type);
}

// ─── Fallible Type ──────────────────────────────────────────────────────

TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, type,
                              "invalid fallible inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FunctionNullable, type,
                              "function types cannot be fallible");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be fallible");
        return nullptr;
    }

    return const_cast<FallibleTypeAST*>(type);
}

// ─── Combined Type ──────────────────────────────────────────────────────

TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, type,
                              "invalid combined inner type");
        return nullptr;
    }

    if (inner->isa<FuncTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_FunctionNullable, type,
                              "function types cannot be combined");
        return nullptr;
    }

    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be combined");
        return nullptr;
    }

    return const_cast<CombinedTypeAST*>(type);
}

// ─── Reference Type ─────────────────────────────────────────────────────

TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidPointerTarget, type,
                              "invalid reference target type");
        return nullptr;
    }

    // ─── Apply Downward Flow Rule ──────────────────────────────────────────
    // References (&T) are strictly scoped. They are allowed to flow downward
    // (into nested calls), but never upward or sideways.
    if (!validateBorrowedContext(type, ctx)) {
        return nullptr;
    }

    // ─── Additional validation: Cannot reference a trait ──────────────────
    if (isTraitType(inner, ctx)) {
        ctx.diagnostics.error(DiagCode::Sem_RefToTrait, type,
                              "cannot take reference to trait type (&Trait)");
        return nullptr;
    }

    return const_cast<RefTypeAST*>(type);
}

// ─── Pointer Type ───────────────────────────────────────────────────────

TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidPointerTarget, type,
                              "invalid pointer target type");
        return nullptr;
    }

    // Raw pointers are sealed conduits - they are always valid structurally
    // FFI compatibility is checked separately in isValidFFIType
    return const_cast<PtrTypeAST*>(type);
}

// ─── Function Type ──────────────────────────────────────────────────────

TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    for (ParamAST* param : type->params) {
        if (!resolveType(param->type, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, param,
                                  "invalid parameter type");
            return nullptr;
        }
    }

    if (type->returnType) {
        TypeAST* returnType = resolveType(type->returnType, ctx);
        if (!returnType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, type,
                                  "invalid return type");
            return nullptr;
        }

        // ─── Check: Function cannot return a borrowed type ────────────────
        // Rule 3: No Borrowed Returns - a function cannot return &T or [_]T
        if (isBorrowedType(returnType)) {
            ctx.diagnostics.error(DiagCode::Sem_ReturnRef, type,
                                  "function cannot return borrowed type (",
                                  debug::typeToString(returnType, ctx.pool),
                                  ") — &T and [_]T cannot escape upward");
            return nullptr;
        }

        if (isTraitType(returnType, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_ReturnTrait, type,
                                  "function cannot return trait type (use a concrete struct instead)");
            return nullptr;
        }

        if (returnType->isa<FuncTypeAST>()) {
            if (!resolveFuncType(returnType->as<FuncTypeAST>(), ctx)) {
                return nullptr;
            }
        }
    }

    return const_cast<FuncTypeAST*>(type);
}

// ─── Trait Resolution ────────────────────────────────────────────────────

const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx) {
    if (!ref) return nullptr;

    const TypeDeclAST* typeDecl = ctx.lookupType(ref->name);
    if (!typeDecl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, ref,
                              "undefined trait '", ctx.pool.lookup(ref->name), "'");
        return nullptr;
    }

    if (!typeDecl->isa<TraitDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotATrait, ref,
                              "'", ctx.pool.lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    const TraitDeclAST* traitDecl = typeDecl->as<TraitDeclAST>();

    if (!ref->genericArgs.empty()) {
        if (ref->genericArgs.size() != traitDecl->genericParams.size()) {
            ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, ref,
                                  "trait '", ctx.pool.lookup(ref->name),
                                  "' expected ", traitDecl->genericParams.size(),
                                  " generic arguments, got ", 
                                  ref->genericArgs.size());
            return nullptr;
        }

        for (const TypePtr arg : ref->genericArgs) {
            if (!resolveType(arg, ctx)) {
                return nullptr;
            }
        }
    }

    return traitDecl;
}

// ─── Callee Resolution ──────────────────────────────────────────────────

const FuncDeclAST* resolveCalleeOrError(const ExprAST* callee, SemaContext& ctx) {
    if (!callee) return nullptr;

    // ─── Case 1: Plain identifier call: `foo(...)` ──────────────────────
    if (callee->isa<IdentifierExprAST>()) {
        const IdentifierExprAST* id = callee->as<IdentifierExprAST>();
        
        if (ctx.isGenericParam(id->name)) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamNotCallable, callee,
                                  "'", ctx.pool.lookup(id->name), "' is a generic type parameter, not a function");
            return nullptr;
        }

        const ValueDeclAST* value = ctx.lookupValue(id->name);
        if (!value) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, callee,
                                  "undefined value '", ctx.pool.lookup(id->name), "'");
            return nullptr;
        }

        if (!value->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                                  "'", ctx.pool.lookup(id->name), "' is not callable");
            return nullptr;
        }

        return value->as<FuncDeclAST>();
    }

    // ─── Case 2: Cross-module call: `module:member(...)` ────────────────
    if (callee->isa<ModuleAccessExprAST>()) {
        const ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        
        const ValueDeclAST* decl = ctx.lookupValueByAlias(access->moduleName, access->memberName);
        if (!decl) {
            // The helper already reported the error (module not found or member not found)
            return nullptr;
        }

        if (!decl->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                                  "'", ctx.pool.lookup(access->moduleName), ":",
                                  ctx.pool.lookup(access->memberName), "' is not callable");
            return nullptr;
        }

        return decl->as<FuncDeclAST>();
    }

    // ─── Case 3: Field access call: `obj.method(...)` ────────────────────
    if (callee->isa<FieldAccessExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                              "field access is not callable (Lucid has no methods)");
        return nullptr;
    }

    // ─── Case 4: Any other callee shape ──────────────────────────────────
    return nullptr;
}

// ─── Self-Reference Detection ───────────────────────────────────────────

void checkLetSelfReference(const ExprAST* expr, InternedString varName, SemaContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            const IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            if (id->name == varName) {
                ctx.diagnostics.error(DiagCode::Sem_SelfReferentialInit, expr,
                                      "let variable '", ctx.pool.lookup(varName),
                                      "' cannot be used in its own initializer");
            }
            return;
        }
        case ASTKind::BinaryExpr: {
            const BinaryExprAST* bin = expr->as<BinaryExprAST>();
            checkLetSelfReference(bin->left, varName, ctx);
            checkLetSelfReference(bin->right, varName, ctx);
            return;
        }
        case ASTKind::UnaryExpr: {
            const UnaryExprAST* unary = expr->as<UnaryExprAST>();
            checkLetSelfReference(unary->operand, varName, ctx);
            return;
        }
        case ASTKind::CallExpr: {
            const CallExprAST* call = expr->as<CallExprAST>();
            checkLetSelfReference(call->callee, varName, ctx);
            for (const ExprAST* arg : call->args) {
                checkLetSelfReference(arg, varName, ctx);
            }
            return;
        }
        case ASTKind::FieldAccessExpr: {
            const FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            checkLetSelfReference(field->object, varName, ctx);
            return;
        }
        case ASTKind::IndexExpr: {
            const IndexExprAST* index = expr->as<IndexExprAST>();
            checkLetSelfReference(index->target, varName, ctx);
            checkLetSelfReference(index->index, varName, ctx);
            return;
        }
        case ASTKind::ArrayLiteralExpr: {
            const ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (const ExprAST* elem : arr->elements) {
                checkLetSelfReference(elem, varName, ctx);
            }
            return;
        }
        case ASTKind::StructLiteralExpr: {
            const StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (const FieldInitAST* init : st->inits) {
                checkLetSelfReference(init->value, varName, ctx);
            }
            return;
        }
        default:
            return;
    }
}

// ─── Struct Self-Reference Validation ─────────────────────────────────────

bool isValidStructSelfReference(const TypeAST* fieldType,
                                 const StructDeclAST* currentStruct,
                                 SemaContext& ctx) {
    if (!fieldType || !currentStruct) return false;

    // ─── Step 1: Unwrap nullable and pointer layers ────────────────────────
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

    // ─── Step 2: Check if the inner type is a NamedType ────────────────────
    if (!innerType->isa<NamedTypeAST>()) {
        return false;  // Not a self-reference
    }

    const NamedTypeAST* named = innerType->as<NamedTypeAST>();

    // ─── Step 3: Check if it references the current struct ─────────────────
    if (named->name != currentStruct->name) {
        return false;  // Not a self-reference
    }

    // ─── Step 4: Check generic arguments match ─────────────────────────────
    if (named->genericArgs.size() != currentStruct->genericParams.size()) {
        return false;  // Different instantiation
    }

    for (size_t i = 0; i < named->genericArgs.size(); ++i) {
        TypeAST* arg = named->genericArgs[i];
        const GenericParamDeclAST* param = currentStruct->genericParams[i];
        
        if (arg->isa<NamedTypeAST>()) {
            NamedTypeAST* argNamed = arg->as<NamedTypeAST>();
            if (argNamed->name != param->name) {
                return false;  // Different generic arguments
            }
        } else {
            return false;  // Not a generic parameter reference
        }
    }

    // ─── Step 5: Validate self-reference rules ─────────────────────────────
    // Self-reference is only valid if it's nullable, a raw pointer, or a slice
    // Note: Slices ([_]T) are borrowed views and cannot be stored in structs
    // This is enforced by the Downward Flow Rule in validateBorrowedContext
    
    // Check if this is a slice self-reference (invalid - borrowed types can't be stored)
    if (innerType->isa<ArrayTypeAST>() && innerType->as<ArrayTypeAST>()->isSlice()) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, fieldType,
                              "slice self-reference ([_]", ctx.pool.lookup(currentStruct->name),
                              ") is not allowed — slices are borrowed views and cannot be stored in structs");
        return false;
    }

    // Non-nullable self-reference is invalid
    if (!isNullable && !isPointer) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, fieldType,
                              "non-nullable self-reference in struct '",
                              ctx.pool.lookup(currentStruct->name),
                              "' (use '?', '*', or '*?' to allow recursion)");
        return false;
    }

    // Self-reference through raw pointer is always allowed
    // Self-reference through nullable is always allowed
    return true;
}

bool isFieldAccessibleOnGenericType(const TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx) {
    if (!genericType || !genericType->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* named = genericType->as<NamedTypeAST>();

    // If it's a generic parameter, check constraints
    if (ctx.isGenericParam(named->name)) {
        const GenericParamDeclAST* param = ctx.lookupGenericParam(named->name);
        if (!param) return false;

        for (const NamedTypeAST* constraint : param->constraints) {
            const TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            for (const TraitFieldDeclAST* field : trait->fields) {
                if (field->name == fieldName) return true;
            }
        }
        return false;
    }

    // Concrete type - check fields
    const TypeDeclAST* decl = ctx.lookupType(named->name);
    if (!decl || !decl->isa<StructDeclAST>()) return false;

    const StructDeclAST* structDecl = decl->as<StructDeclAST>();
    for (const FieldDeclAST* field : structDecl->fields) {
        if (field->name == fieldName) return true;
    }

    return false;
}

const TypeAST* getFieldTypeOnGenericType(const TypeAST* genericType,
                                         InternedString fieldName,
                                         SemaContext& ctx) {
    if (!genericType || !genericType->isa<NamedTypeAST>()) return nullptr;
    const NamedTypeAST* named = genericType->as<NamedTypeAST>();

    if (ctx.isGenericParam(named->name)) {
        const GenericParamDeclAST* param = ctx.lookupGenericParam(named->name);
        if (!param) return nullptr;

        for (const NamedTypeAST* constraint : param->constraints) {
            const TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            for (const TraitFieldDeclAST* field : trait->fields) {
                if (field->name == fieldName) {
                    return field->type;
                }
            }
        }
        return nullptr;
    }

    const TypeDeclAST* decl = ctx.lookupType(named->name);
    if (!decl || !decl->isa<StructDeclAST>()) return nullptr;

    const StructDeclAST* structDecl = decl->as<StructDeclAST>();
    for (const FieldDeclAST* field : structDecl->fields) {
        if (field->name == fieldName) {
            return field->type;
        }
    }

    return nullptr;
}

// ─── Type Narrowing Helpers ──────────────────────────────────────────────

const TypeAST* unwrapFutureType(const TypeAST* type) {
    if (!type) return nullptr;
    if (type->isa<FutureTypeAST>()) {
        return type->as<FutureTypeAST>()->inner;
    }
    return nullptr;
}

const TypeAST* unwrapThreadType(const TypeAST* type) {
    if (!type) return nullptr;
    if (type->isa<ThreadTypeAST>()) {
        return type->as<ThreadTypeAST>()->inner;
    }
    return nullptr;
}

// ─── Downward Flow Rule Validation ──────────────────────────────────────

bool isBorrowedType(const TypeAST* type) {
    if (!type) return false;
    
    // &T is a borrowed type
    if (type->isa<RefTypeAST>()) {
        return true;
    }
    
    // [_]T is a borrowed type (slice)
    if (type->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* array = type->as<ArrayTypeAST>();
        if (array->isSlice()) {
            return true;
        }
    }
    
    return false;
}

bool validateBorrowedContext(const TypeAST* type, SemaContext& ctx) {
    if (!type || !isBorrowedType(type)) {
        return true;
    }

    // ─── Rule 1: No Struct Storage ─────────────────────────────────────────
    // A borrowed type cannot be stored in a struct field
    const TypeDeclAST* currentType = ctx.currentDefiningType();
    if (currentType && currentType->isa<StructDeclAST>()) {
        const char* typeName = type->isa<RefTypeAST>() ? "reference (&T)" : "slice ([_]T)";
        ctx.diagnostics.error(DiagCode::Sem_RefInStruct, type,
                              "borrowed type ", typeName,
                              " cannot be stored in struct fields");
        return false;
    }

    // ─── Rule 2: No Array/Slice Storage ────────────────────────────────────
    // A borrowed type cannot be an element of an array or slice
    // This is checked in resolveArrayType, but we also check the context here
    // The caller should have already checked this
    
    // ─── Rule 3: No Borrowed Returns ──────────────────────────────────────
    // A borrowed type cannot be returned from a function
    // This is checked in resolveFuncType for the return type
    
    // ─── Rule 4: No Closure Capture ──────────────────────────────────────
    // A borrowed type cannot be captured by a closure
    // This is checked in resolveAnonFuncExpr and resolveFuncDecl
    
    return true;
}

} // namespace sema