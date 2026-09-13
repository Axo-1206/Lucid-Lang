/// @file SemaResolve.cpp
/// @brief Implementation of type resolution functions.

#include "SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/ASTStrings.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "sema/context/Generic.hpp"

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

// ─── Primitive Type ──────────────────────────────────────────────────────

TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx) {
    (void)ctx;
    return type;
}

// ─── Named Type ──────────────────────────────────────────────────────────
//
// Resolves a `NamedTypeAST` — an identifier written in a type position.
//
// The four kinds of declaration a type name can refer to:
//
//   - GenericParamDeclAST — a `T` from an enclosing `<T>` list. Handled
//     first: the reference is valid only inside the generic declaration
//     that introduced the parameter, and the resolved decl is the
//     parameter itself.
//
//   - TraitDeclAST — a trait name, valid only as a generic constraint.
//     Anywhere else it is a diagnostic. (The constraint context is
//     checked in resolveTypeDecl before we reach here.)
//
//   - StructDeclAST — the ordinary case. With generic args, the named
//     type is an instantiation request: resolve (or retrieve from
//     cache) the specialized struct and bind it as the resolved decl.
//     Without generic args, the name binds to the template directly.
//
//   - EnumDeclAST — enums are not generic. Reject any generic args and
//     bind the enum declaration directly.
//
// Note on `isa<>`: in this codebase `isa<T>()` compares `node->kind`
// against `T::staticKind`, so it is true only for the *exact* runtime
// kind, not for subclasses of `T`. Every concrete declaration overrides
// `staticKind`, so `isa<TypeDeclAST>()` and `isa<ValueDeclAST>()` are
// false for every real node. Always check for the concrete kind
// (`isa<StructDeclAST>()`, `isa<EnumDeclAST>()`), never for the abstract
// base. See the note above `resolveGenericInstantiation`'s return-type
// handling if `isa<Base>()` is ever made to walk the hierarchy.
TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx) {
    if (!type) return nullptr;

    // ─── Step 1: Resolve the name to a declaration ────────────────────
    TypeDeclAST* decl = resolveTypeDecl(type, ctx);
    if (!decl) {
        return nullptr;
    }

    // ─── Step 2: Generic parameter reference ──────────────────────────
    //
    // `T` inside the body of `const f<T> (...)`. The name refers to the
    // parameter; the resolved decl is the GenericParamDeclAST itself.
    //
    // A generic parameter cannot be instantiated — `<T>` is already the
    // parameter, and there is no family behind it to specialize. Reject
    // any generic args written on the reference.
    if (decl->isa<GenericParamDeclAST>()) {
        if (!type->genericArgs.empty()) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                  "generic parameter '", ctx.pool.lookup(type->name),
                                  "' cannot have generic arguments");
            return nullptr;
        }
        return type;
    }

    // ─── Step 3: Trait reference ──────────────────────────────────────
    //
    // A trait name. The context check (is it in a GenericConstraint?)
    // happened inside resolveTypeDecl. A trait is never instantiable and
    // never a value; if resolveTypeDecl accepted the reference, there is
    // nothing further to validate here.
    if (decl->isa<TraitDeclAST>()) {
        return type;
    }

    // ─── Step 4: Validate generic arguments against the template ──────
    //
    // For a struct: check arity, resolve each arg, validate trait
    // constraints. For an enum: reject any generic args. This runs
    // before the struct-instantiation branch so both struct and enum
    // share the arity/args validation.
    if (!validateGenericInstantiation(type, ctx)) {
        return nullptr;
    }

    // ─── Step 5: Struct reference ─────────────────────────────────────
    if (decl->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = decl->as<StructDeclAST>();

        // ─── 5a. Non-generic struct — bind the template directly ──────
        if (type->genericArgs.empty()) {
            type->resolvedDecl = structDecl;
            return type;
        }

        // ─── 5b. Generic struct — check storage map first ────────────────────
        //
        // Canonicalize the args before anything else. Two `Box<int>` at different
        // call sites produce two distinct PrimitiveTypeAST(Int) nodes from the
        // parser; the storage map's key is pointer-identity on canonicalized args,
        // so canonicalization must happen before the lookup or the key will not
        // match what was registered.
        std::vector<TypeAST*> canonicalArgsList;
        ArenaSpan<TypeAST*> canonicalArgs = canonicalizeTypeArgList(type->genericArgs, ctx);

        // ─── Check the structural storage map ─────────────────────────────────
        StructDeclAST* resolvedStruct =
            ctx.getGenericTypeInstantiation(structDecl->name, canonicalArgs);

        if (!resolvedStruct) {
            // Miss — instantiate. resolveGenericInstantiation already
            // canonicalizes internally and registers in the structural map via
            // finalizeInstantiatedStruct, so we do not need to insert here.
            GenericResolution resolution = resolveGenericInstantiation(
                structDecl, canonicalArgs, ctx);

            if (!resolution.resolvedDecl) {
                return nullptr;
            }
            if (!resolution.resolvedDecl->isa<StructDeclAST>()) {
                ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, type,
                                    "generic instantiation of '", ctx.pool.lookup(type->name),
                                    "' did not produce a struct declaration");
                return nullptr;
            }
            resolvedStruct = resolution.resolvedDecl->as<StructDeclAST>();
        }

        // ─── Canonicalize the NamedTypeAST's own args and bind the decl ───────
        //
        // Write the canonical args back onto `type` itself, not just into the
        // lookup. Two `NamedTypeAST("Box", [int])` nodes must have identical
        // `genericArgs` pointers, not just identical `resolvedDecl` — otherwise
        // any comparison that falls back on args (rather than resolvedDecl)
        // still sees two different nodes.
        type->genericArgs = canonicalArgs;
        type->resolvedDecl = resolvedStruct;
        return type;
    }

    // ─── Step 6: Enum reference ───────────────────────────────────────
    //
    // Enums are never generic. `validateGenericInstantiation` already
    // rejected any generic args on an enum reference, so by the time
    // we're here the reference is plain and binds directly.
    if (decl->isa<EnumDeclAST>()) {
        type->resolvedDecl = decl;
        return type;
    }

    // ─── Step 7: Anything else ────────────────────────────────────────
    //
    // resolveTypeDecl only ever returns one of the four kinds above.
    // Reaching this point means a new kind was added without extending
    // this function — a compiler bug, not a user error. Emit an
    // internal-style diagnostic and return null.
    ctx.diagnostics.error(DiagCode::Sem_UnknownType, type,
                          "type '", ctx.pool.lookup(type->name),
                          "' resolves to an unsupported declaration kind");
    return nullptr;
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