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

TypeAST* resolveType(TypeAST* type, SemaContext& ctx) {
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

TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx) {
    (void)ctx;
    return type;
}

// ─── Named Type ──────────────────────────────────────────────────────────

TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── 1. Resolve the declaration if not already set ─────────────────────
    if (!type->resolvedDecl) {
        TypeDeclAST* decl = ctx.lookupTypeDecl(type->name);
        if (!decl) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedType, type,
                                  "undefined type '", ctx.pool.lookup(type->name), "'");
            return nullptr;
        }
        type->resolvedDecl = decl;
    }

    // ─── 2. Generic parameters can't have generic arguments ───────────────
    TypeDeclAST* decl = type->resolvedDecl;
    if (decl->isa<GenericParamDeclAST>()) {
        if (!type->genericArgs.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "generic parameter '", ctx.pool.lookup(type->name),
                                  "' cannot have generic arguments");
            return nullptr;
        }
        return type;
    }

    // ─── 3. Resolve generic arguments if present ─────────────────────────
    if (!type->genericArgs.empty()) {
        // Check which kind of declaration we have
        size_t expectedParams = 0;
        bool isGeneric = false;

        if (decl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = decl->as<StructDeclAST>();
            expectedParams = structDecl->genericParams.size();
            isGeneric = !structDecl->genericParams.empty();
        } else if (decl->isa<TraitDeclAST>()) {
            TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            expectedParams = traitDecl->genericParams.size();
            isGeneric = !traitDecl->genericParams.empty();
        } else if (decl->isa<EnumDeclAST>()) {
            // Enums cannot be generic
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "enum '", ctx.pool.lookup(type->name), "' is not generic");
            return nullptr;
        } else {
            ctx.diagnostics.error(DiagCode::Sem_UnknownType, type,
                                  "unknown type declaration kind");
            return nullptr;
        }

        if (!isGeneric) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "type '", ctx.pool.lookup(type->name), "' is not generic");
            return nullptr;
        }

        if (type->genericArgs.size() != expectedParams) {
            ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                                  "type '", ctx.pool.lookup(type->name),
                                  "' expected ", expectedParams,
                                  " generic arguments, got ", 
                                  type->genericArgs.size());
            return nullptr;
        }

        // Resolve each generic argument type
        // NOTE: type->genericArgs is an ArenaSpan<TypeAST*>, so we iterate
        // over it directly. The elements are already TypeAST* and we need
        // to resolve them.
        for (TypeAST* arg : type->genericArgs) {
            if (!resolveType(arg, ctx)) {
                return nullptr;
            }
        }

        // Validate constraints for generic arguments
        if (decl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (!validateGenericArguments(type->genericArgs, structDecl->genericParams, type, ctx)) {
                return nullptr;
            }
        } else if (decl->isa<TraitDeclAST>()) {
            TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (!validateGenericArguments(type->genericArgs, traitDecl->genericParams, type, ctx)) {
                return nullptr;
            }
        }
    } else {
        // Check if the type requires generic arguments
        bool requiresGeneric = false;
        if (decl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = decl->as<StructDeclAST>();
            if (!structDecl->genericParams.empty()) {
                requiresGeneric = true;
            }
        } else if (decl->isa<TraitDeclAST>()) {
            TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();
            if (!traitDecl->genericParams.empty()) {
                requiresGeneric = true;
            }
        }

        if (requiresGeneric) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, type,
                                  "type '", ctx.pool.lookup(type->name),
                                  "' requires generic arguments");
            return nullptr;
        }
    }

    return type;
}

// ─── Module Type Access ──────────────────────────────────────────────────

TypeAST* resolveModuleTypeAccess(ModuleTypeAccessAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── Step 1: Look up the type in the module by alias ──────────────────
    TypeDeclAST* decl = ctx.lookupTypeByAlias(type->moduleName, type->typeName);
    if (!decl) {
        // The helper already reported the error (module not found or member not found)
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

    // ─── Step 3: Get the canonical NamedTypeAST with generic args ─────────
    // The cache key includes genericArgs, so Vec2<int> and Vec2<float>
    // are stored separately. This prevents type corruption across different
    // instantiations of the same generic type name.
    NamedTypeAST* resolvedType = ctx.getNamedType(type->typeName, type->genericArgs);
    
    // ─── Step 4: Transfer semantic data ────────────────────────────────────
    // The resolvedDecl is set here, not in the cache key, because it's a
    // semantic property (the declaration we resolved to), not a syntactic
    // property of the type name. This is safe because the cache key only
    // includes the name and generic args - the resolvedDecl is set once
    // and never changes for a given instantiation.
    resolvedType->resolvedDecl = decl;
    resolvedType->loc = type->loc;

    // ─── Step 5: Delegate to resolveNamedType for validation ──────────────
    // Since resolvedDecl is already set, this skips the lookup phase and
    // only validates generic arguments, arity, and constraints.
    return resolveNamedType(resolvedType, ctx);
}

// ─── Array Type ──────────────────────────────────────────────────────────

TypeAST* resolveArrayType(ArrayTypeAST* type, SemaContext& ctx) {
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
        ArrayTypeAST* innerArray = element->as<ArrayTypeAST>();
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

    return type;
}

// ─── Nullable Type ──────────────────────────────────────────────────────

TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx) {
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

    // This should not be ran, the parser should create an array with nullable elements
    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be nullable (use empty array instead)");
        return nullptr;
    }

    return type;
}

// ─── Fallible Type ──────────────────────────────────────────────────────

TypeAST* resolveFallibleType(FallibleTypeAST* type, SemaContext& ctx) {
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

    // This should not be ran, the parser should create an array with fallible elements
    if (inner->isa<ArrayTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ArrayNullable, type,
                              "array types cannot be fallible");
        return nullptr;
    }

    return type;
}

// ─── Combined Type ──────────────────────────────────────────────────────

TypeAST* resolveCombinedType(CombinedTypeAST* type, SemaContext& ctx) {
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

    return type;
}

// ─── Reference Type ─────────────────────────────────────────────────────

TypeAST* resolveRefType(RefTypeAST* type, SemaContext& ctx) {
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

    return type;
}

// ─── Pointer Type ───────────────────────────────────────────────────────

TypeAST* resolvePtrType(PtrTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) {
        ctx.diagnostics.error(DiagCode::Sem_InvalidPointerTarget, type,
                              "invalid pointer target type");
        return nullptr;
    }

    // Raw pointers are sealed conduits - they are always valid structurally
    // FFI compatibility is checked separately in isValidFFIType
    return type;
}

// ─── Function Type ──────────────────────────────────────────────────────

TypeAST* resolveFuncType(FuncTypeAST* type, SemaContext& ctx) {
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

    return type;
}

// ─── Trait Resolution ────────────────────────────────────────────────────

TraitDeclAST* resolveTraitRef(NamedTypeAST* ref, SemaContext& ctx) {
    if (!ref) return nullptr;

    // ─── Step 1: Resolve the named type ────────────────────────────────────
    // This will set resolvedDecl and validate generic arguments
    TypeAST* resolved = resolveNamedType(ref, ctx);
    if (!resolved) return nullptr;

    // ─── Step 2: Check if it's a trait ─────────────────────────────────────
    TypeDeclAST* decl = ref->resolvedDecl;
    if (!decl) return nullptr;

    if (!decl->isa<TraitDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotATrait, ref,
                              "'", ctx.pool.lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    return decl->as<TraitDeclAST>();
}

// ─── Callee Resolution ──────────────────────────────────────────────────

FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx) {
    if (!callee) return nullptr;

    // ─── Case 1: Plain identifier call: `foo(...)` ──────────────────────
    if (callee->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = callee->as<IdentifierExprAST>();
        
        if (ctx.isGenericParam(id->name)) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamNotCallable, callee,
                                  "'", ctx.pool.lookup(id->name), "' is a generic type parameter, not a function");
            return nullptr;
        }

        ValueDeclAST* value = ctx.lookupValue(id->name);
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
        ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        
        ValueDeclAST* decl = ctx.lookupValueByAlias(access->moduleName, access->memberName);
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

void checkLetSelfReference(ExprAST* expr, InternedString varName, SemaContext& ctx) {
    if (!expr) return;

    switch (expr->kind) {
        case ASTKind::IdentifierExpr: {
            IdentifierExprAST* id = expr->as<IdentifierExprAST>();
            if (id->name == varName) {
                ctx.diagnostics.error(DiagCode::Sem_SelfReferentialInit, expr,
                                      "let variable '", ctx.pool.lookup(varName),
                                      "' cannot be used in its own initializer");
            }
            return;
        }
        case ASTKind::BinaryExpr: {
            BinaryExprAST* bin = expr->as<BinaryExprAST>();
            checkLetSelfReference(bin->left, varName, ctx);
            checkLetSelfReference(bin->right, varName, ctx);
            return;
        }
        case ASTKind::UnaryExpr: {
            UnaryExprAST* unary = expr->as<UnaryExprAST>();
            checkLetSelfReference(unary->operand, varName, ctx);
            return;
        }
        case ASTKind::CallExpr: {
            CallExprAST* call = expr->as<CallExprAST>();
            checkLetSelfReference(call->callee, varName, ctx);
            for (ExprAST* arg : call->args) {
                checkLetSelfReference(arg, varName, ctx);
            }
            return;
        }
        case ASTKind::FieldAccessExpr: {
            FieldAccessExprAST* field = expr->as<FieldAccessExprAST>();
            checkLetSelfReference(field->object, varName, ctx);
            return;
        }
        case ASTKind::IndexExpr: {
            IndexExprAST* index = expr->as<IndexExprAST>();
            checkLetSelfReference(index->target, varName, ctx);
            checkLetSelfReference(index->index, varName, ctx);
            return;
        }
        case ASTKind::ArrayLiteralExpr: {
            ArrayLiteralExprAST* arr = expr->as<ArrayLiteralExprAST>();
            for (ExprAST* elem : arr->elements) {
                checkLetSelfReference(elem, varName, ctx);
            }
            return;
        }
        case ASTKind::StructLiteralExpr: {
            StructLiteralExprAST* st = expr->as<StructLiteralExprAST>();
            for (FieldInitAST* init : st->inits) {
                checkLetSelfReference(init->value, varName, ctx);
            }
            return;
        }
        default:
            return;
    }
}

// ─── Struct Self-Reference Validation ─────────────────────────────────────

bool isValidStructSelfReference(TypeAST* fieldType,
                                 StructDeclAST* currentStruct,
                                 SemaContext& ctx) {
    if (!fieldType || !currentStruct) return false;

    // ─── Step 1: Unwrap nullable and pointer layers ────────────────────────
    bool isNullable = false;
    bool isPointer = false;
    TypeAST* innerType = fieldType;

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

    NamedTypeAST* named = innerType->as<NamedTypeAST>();

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
        GenericParamDeclAST* param = currentStruct->genericParams[i];
        
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

bool isFieldAccessibleOnGenericType(TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx) {
    if (!genericType || !genericType->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = genericType->as<NamedTypeAST>();

    // ─── Step 1: Ensure the type is resolved ──────────────────────────────
    if (!named->resolvedDecl) {
        resolveNamedType(named, ctx);
    }

    // ─── Step 2: Check if it's a generic parameter ─────────────────────────
    if (named->resolvedDecl && named->resolvedDecl->isa<GenericParamDeclAST>()) {
        GenericParamDeclAST* param = static_cast<GenericParamDeclAST*>(named->resolvedDecl);
        
        for (NamedTypeAST* constraint : param->constraints) {
            TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            for (TraitFieldDeclAST* field : trait->fields) {
                if (field->name == fieldName) return true;
            }
        }
        return false;
    }

    // ─── Step 3: Concrete type - check fields ─────────────────────────────
    TypeDeclAST* decl = named->resolvedDecl;
    if (!decl || !decl->isa<StructDeclAST>()) return false;

    StructDeclAST* structDecl = decl->as<StructDeclAST>();
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->name == fieldName) return true;
    }

    return false;
}

TypeAST* getFieldTypeOnGenericType(TypeAST* genericType,
                                   InternedString fieldName,
                                   SemaContext& ctx) {
    if (!genericType || !genericType->isa<NamedTypeAST>()) return nullptr;
    NamedTypeAST* named = genericType->as<NamedTypeAST>();

    // ─── Step 1: Ensure the type is resolved ──────────────────────────────
    if (!named->resolvedDecl) {
        resolveNamedType(named, ctx);
    }

    // ─── Step 2: Check if it's a generic parameter ─────────────────────────
    if (named->resolvedDecl && named->resolvedDecl->isa<GenericParamDeclAST>()) {
        GenericParamDeclAST* param = static_cast<GenericParamDeclAST*>(named->resolvedDecl);
        
        for (NamedTypeAST* constraint : param->constraints) {
            TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            for (TraitFieldDeclAST* field : trait->fields) {
                if (field->name == fieldName) {
                    return field->type;
                }
            }
        }
        return nullptr;
    }

    // ─── Step 3: Concrete type - check fields ─────────────────────────────
    TypeDeclAST* decl = named->resolvedDecl;
    if (!decl || !decl->isa<StructDeclAST>()) return nullptr;

    StructDeclAST* structDecl = decl->as<StructDeclAST>();
    for (FieldDeclAST* field : structDecl->fields) {
        if (field->name == fieldName) {
            return field->type;
        }
    }

    return nullptr;
}

} // namespace sema