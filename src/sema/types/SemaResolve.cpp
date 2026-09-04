/// @file SemaResolve.cpp
/// @brief Implementation of type resolution functions.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace sema {

// ─── Helper: Resolve declaration for a named type ──────────────────────

static TypeDeclAST* resolveTypeDecl(NamedTypeAST* type, SemaContext& ctx) {
    // If already resolved, return it
    if (type->resolvedDecl) {
        return type->resolvedDecl;
    }
    
    // Look up the declaration
    TypeDeclAST* decl = ctx.lookupTypeDecl(type->name);
    if (!decl) {
        ctx.diagnostics.error(DiagCode::Sem_UndefinedType, type,
                              "undefined type '", ctx.pool.lookup(type->name), "'");
        return nullptr;
    }
    
    // Check if it's a generic parameter
    if (decl->isa<GenericParamDeclAST>()) {
        type->resolvedDecl = decl;
        return decl;
    }
    
    // Check if this is a trait being used in an invalid context
    if (decl->isa<TraitDeclAST>()) {
        // Traits are only valid as generic constraints
        if (!ctx.stack.isInside(ContextKind::GenericConstraint)) {
            ctx.diagnostics.error(DiagCode::Sem_TraitInvalidContext, type,
                                  "trait '", ctx.pool.lookup(type->name), 
                                  "' can only be used as a generic constraint");
            return nullptr;
        }
        type->resolvedDecl = decl;
        return decl;
    }
    
    // Store the resolved declaration
    type->resolvedDecl = decl;
    return decl;
}

// ─── Helper: Validate generic instantiation ────────────────────────────

static bool validateGenericInstantiation(NamedTypeAST* type, SemaContext& ctx) {
    if (!type->resolvedDecl) return false;
    TypeDeclAST* decl = type->resolvedDecl;
    
    // Generic parameters are handled elsewhere
    if (decl->isa<GenericParamDeclAST>()) {
        if (!type->genericArgs.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "generic parameter '", ctx.pool.lookup(type->name),
                                  "' cannot have generic arguments");
            return false;
        }
        return true;
    }
    
    // Get expected parameter count
    size_t expectedParams = 0;
    ArenaSpan<GenericParamDeclAST*> genericParams;
    
    if (auto* structDecl = decl->as<StructDeclAST>()) {
        genericParams = structDecl->genericParams;
        expectedParams = genericParams.size();
    } else if (auto* traitDecl = decl->as<TraitDeclAST>()) {
        genericParams = traitDecl->genericParams;
        expectedParams = genericParams.size();
    } else if (decl->isa<EnumDeclAST>()) {
        // Enums are not generic
        if (!type->genericArgs.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "enum '", ctx.pool.lookup(type->name), "' is not generic");
            return false;
        }
        return true;
    } else {
        ctx.diagnostics.error(DiagCode::Sem_UnknownType, type, "unknown type declaration kind");
        return false;
    }
    
    // Check if generic arguments are required but missing
    bool requiresGeneric = expectedParams > 0;
    if (requiresGeneric && type->genericArgs.empty()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericParamRequired, type,
                              "type '", ctx.pool.lookup(type->name),
                              "' requires ", expectedParams, " generic arguments");
        return false;
    }
    
    // Check arity
    if (type->genericArgs.size() != expectedParams) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, type,
                              "type '", ctx.pool.lookup(type->name),
                              "' expected ", expectedParams,
                              " generic arguments, got ", type->genericArgs.size());
        return false;
    }
    
    // Resolve each argument
    for (TypeAST* arg : type->genericArgs) {
        if (!resolveType(arg, ctx)) {
            return false;
        }
    }
    
    // Validate constraints
    if (!validateGenericArguments(type->genericArgs, genericParams, type, ctx)) {
        return false;
    }
    
    return true;
}

// ─── Main Resolution Entry Point ─────────────────────────────────────────

TypeAST* resolveType(TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::PrimitiveType:     
            return type;  // Already resolved
            
        case ASTKind::NamedType:         
            return resolveNamedType(type->as<NamedTypeAST>(), ctx);
            
        case ASTKind::SimdType:
        case ASTKind::ArenaType:
        case ASTKind::ArenaDescriptorType:
            return resolveBuiltinType(type, ctx);
            
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
            ctx.diagnostics.error(DiagCode::Sem_UnknownType, type, "unknown type");
            return nullptr;
    }
}

// ─── Named Type ──────────────────────────────────────────────────────────

TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── Step 1: Resolve the declaration ─────────────────────────────────────
    TypeDeclAST* decl = resolveTypeDecl(type, ctx);
    if (!decl) {
        return nullptr;
    }
    
    // If it's a generic parameter, no further validation needed
    if (decl->isa<GenericParamDeclAST>()) {
        if (!type->genericArgs.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "generic parameter '", ctx.pool.lookup(type->name),
                                  "' cannot have generic arguments");
            return nullptr;
        }
        return type;
    }
    
    // If it's a trait, we already validated context in resolveTypeDecl
    if (decl->isa<TraitDeclAST>()) {
        return type;
    }

    // ─── Step 2: Validate generic arguments ──────────────────────────────────
    if (!validateGenericInstantiation(type, ctx)) {
        return nullptr;
    }

    return type;
}

// ─── Built-in Type Resolution ────────────────────────────────────────────

TypeAST* resolveBuiltinType(TypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    switch (type->kind) {
        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            
            // ─── Resolve the element type ──────────────────────────────────────
            // If the element type is a generic parameter, resolveNamedType will
            // return a NamedTypeAST with isGenericParam = true.
            // validateSimdType will then reject it.
            if (simd->elementType) {
                simd->elementType = resolveType(simd->elementType, ctx);
                if (!simd->elementType) {
                    return nullptr;
                }
            }
            
            // ─── Validate the Simd type ──────────────────────────────────────
            // This will check that:
            //   1. elementType is not a generic parameter
            //   2. elementType is not another Simd type
            //   3. elementType is a numeric primitive
            //   4. laneCount > 0
            if (!validateSimdType(simd, ctx)) {
                return nullptr;
            }
            
            return simd;
        }
        
        case ASTKind::ArenaType: {
            // Arena is already resolved - just return it
            // resolveVarDecl in SemaDecl.cpp will resolve it
            return type;
        }
        
        case ASTKind::ArenaDescriptorType: {
            // ArenaDescriptor is already resolved - just return it
            return type;
        }
        
        default:
            ctx.diagnostics.error(DiagCode::Sem_UnknownType, type,
                                  "unknown built-in type");
            return nullptr;
    }
}

// ─── Module Type Access ──────────────────────────────────────────────────

TypeAST* resolveModuleTypeAccess(ModuleTypeAccessAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Look up the type in the module by alias
    TypeDeclAST* decl = ctx.lookupTypeByAlias(type->moduleName, type->typeName);
    if (!decl) {
        return nullptr;  // Error already reported
    }

    // Check if the type is exported
    if (!ctx.isTypeExported(decl)) {
        ctx.diagnostics.error(DiagCode::Sem_PrivateMember, type,
                              "type '", ctx.pool.lookup(type->typeName), "' in module '",
                              ctx.pool.lookup(type->moduleName), "' is not exported");
        return nullptr;
    }

    // Get the canonical NamedTypeAST
    NamedTypeAST* resolvedType = ctx.getNamedType(type->typeName, type->genericArgs);
    resolvedType->resolvedDecl = decl;
    resolvedType->loc = type->loc;

    // Delegate to resolveNamedType for validation
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

    // Arena cannot be stored in arrays
    if (isArenaType(element)) {
        ctx.diagnostics.error(DiagCode::Sem_RefInArray, type,
                              "array element cannot be of type Arena");
        return nullptr;
    }

    // Reference types cannot be stored in arrays
    if (element->isa<RefTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_RefInArray, type,
                              "reference type (&T) cannot be stored in an array");
        return nullptr;
    }

    // Slice types cannot be stored in arrays
    if (element->isa<ArrayTypeAST>() && element->as<ArrayTypeAST>()->isSlice()) {
        ctx.diagnostics.error(DiagCode::Sem_RefInArray, type,
                              "slice type ([_]T) cannot be stored in an array element");
        return nullptr;
    }

    // Apply Downward Flow Rule to slices
    if (type->isSlice() && !validateBorrowedContext(type, ctx)) {
        return nullptr;
    }

    return type;
}

// ─── Nullable Type ──────────────────────────────────────────────────────

TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) return nullptr;

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

    return type;
}

// ─── Fallible Type ──────────────────────────────────────────────────────

TypeAST* resolveFallibleType(FallibleTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) return nullptr;

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

    return type;
}

// ─── Combined Type ──────────────────────────────────────────────────────

TypeAST* resolveCombinedType(CombinedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    TypeAST* inner = resolveType(type->inner, ctx);
    if (!inner) return nullptr;

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

    if (!validateBorrowedContext(type, ctx)) {
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

    return type;
}

// ─── Function Type ──────────────────────────────────────────────────────

TypeAST* resolveFuncType(FuncTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // Validate parameter types
    for (ParamAST* param : type->params) {
        if (!resolveType(param->type, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, param,
                                  "invalid parameter type");
            return nullptr;
        }

        if (isArenaType(param->type)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidParamType, param,
                                  "parameter '", ctx.pool.lookup(param->name),
                                  "' cannot be of type Arena (use &Arena)");
            return nullptr;
        }
    }

    // Validate return type
    if (type->returnType) {
        TypeAST* returnType = resolveType(type->returnType, ctx);
        if (!returnType) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, type,
                                  "invalid return type");
            return nullptr;
        }

        if (isArenaType(returnType)) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidReturnType, type,
                                  "function cannot return Arena by value");
            return nullptr;
        }

        if (isBorrowedType(returnType)) {
            ctx.diagnostics.error(DiagCode::Sem_ReturnRef, type,
                                  "function cannot return borrowed type");
            return nullptr;
        }

        // Recursively resolve curried return types
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

    // Resolve the named type
    TypeAST* resolved = resolveNamedType(ref, ctx);
    if (!resolved) return nullptr;

    // Check if it's a trait
    TypeDeclAST* decl = ref->resolvedDecl;
    if (!decl || !decl->isa<TraitDeclAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotATrait, ref,
                              "'", ctx.pool.lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    return decl->as<TraitDeclAST>();
}

// ─── Callee Resolution ──────────────────────────────────────────────────

FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx) {
    if (!callee) return nullptr;

    // Plain identifier call: `foo(...)`
    if (callee->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = callee->as<IdentifierExprAST>();
        
        if (ctx.isGenericParam(id->name)) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamNotCallable, callee,
                                  "'", ctx.pool.lookup(id->name), "' is a type parameter");
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

    // Cross-module call: `module:member(...)`
    if (callee->isa<ModuleAccessExprAST>()) {
        ModuleAccessExprAST* access = callee->as<ModuleAccessExprAST>();
        
        ValueDeclAST* decl = ctx.lookupValueByAlias(access->moduleName, access->memberName);
        if (!decl) return nullptr;

        if (!decl->isa<FuncDeclAST>()) {
            ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                                  "'", ctx.pool.lookup(access->moduleName), ":",
                                  ctx.pool.lookup(access->memberName), "' is not callable");
            return nullptr;
        }

        return decl->as<FuncDeclAST>();
    }

    // Field access call: `obj.method(...)` - not allowed
    if (callee->isa<FieldAccessExprAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_NotCallable, callee,
                              "field access is not callable (Lucid has no methods)");
        return nullptr;
    }

    return nullptr;
}

} // namespace sema