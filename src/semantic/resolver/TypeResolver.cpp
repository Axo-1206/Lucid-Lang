/**
 * @file TypeResolver.cpp
 * @brief Implementation of type resolution pass.
 */

#include "TypeResolver.hpp"
#include "ast/DeclAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace luc {

// ============================================================================
// Constructor
// ============================================================================

TypeResolver::TypeResolver(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("TypeResolver constructed");
}

// ============================================================================
// Core Resolution
// ============================================================================

TypeAST* TypeResolver::resolve(TypeAST* type) {
    if (!type) return nullptr;
    
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolve: kind=" 
                             << LucDebug::kindToString(type->kind));
    
    // If already resolved (valueType on a declaration), return as-is
    // For now, just dispatch based on kind
    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return resolvePrimitive(type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return resolveNamed(type->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return resolveNullable(type->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return resolveResult(type->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return resolveArray(type->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return resolveRef(type->as<RefTypeAST>());
        case ASTKind::PtrType:
            return resolvePtr(type->as<PtrTypeAST>());
        case ASTKind::FuncType:
            return resolveFunc(type->as<FuncTypeAST>());
        default:
            LUC_LOG_SEMANTIC("TypeResolver::resolve: unknown type kind");
            return type;
    }
}

TypeAST* TypeResolver::resolveTypeAlias(TypeAliasDeclAST* alias) {
    if (!alias) return nullptr;
    
    // Check if already resolved
    if (alias->resolvedType) {
        return alias->resolvedType;
    }
    
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveTypeAlias: " 
                             << ctx_.pool.lookup(alias->name));
    
    // Recursively resolve the aliased type
    TypeAST* resolved = resolve(alias->aliasedType);
    
    if (!resolved) {
        ctx_.error(alias->loc, DiagCode::E2001,
                   "cannot resolve type alias '", ctx_.pool.lookup(alias->name), "'");
        return nullptr;
    }
    
    // Cache the resolved type
    alias->resolvedType = resolved;
    alias->valueType = resolved;  // For ValueDeclAST compatibility
    
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveTypeAlias: " 
                             << ctx_.pool.lookup(alias->name) 
                             << " -> " << LucDebug::kindToString(resolved->kind));
    
    return resolved;
}

// ============================================================================
// Type Resolution Helpers
// ============================================================================

TypeAST* TypeResolver::resolvePrimitive(PrimitiveTypeAST* prim) {
    // Primitive types are already resolved
    return prim;
}

TypeAST* TypeResolver::resolveNamed(NamedTypeAST* named) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveNamed: " 
                             << ctx_.pool.lookup(named->name));
    
    // Look up the name in the type namespace
    TypeDeclAST* decl = ctx_.scope.lookupType(named->name);
    if (!decl) {
        ctx_.error(named->loc, DiagCode::E2001,
                   "undefined type '", ctx_.pool.lookup(named->name), "'");
        return nullptr;
    }
    
    // If it's a type alias, resolve to underlying type
    if (auto* alias = decl->as<TypeAliasDeclAST>()) {
        TypeAST* resolved = resolveTypeAlias(alias);
        if (!resolved) return nullptr;
        
        // Copy generic arguments from the named type to the resolved type?
        // For now, just return the resolved type
        return resolved;
    }
    
    // Ensure selfType exists
    ensureSelfType(decl);
    
    // Return the selfType reference
    return decl->selfType;
}

TypeAST* TypeResolver::resolveNullable(NullableTypeAST* nullable) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveNullable");
    
    TypeAST* inner = resolve(nullable->inner);
    if (!inner) return nullptr;
    
    nullable->inner = inner;
    return nullable;
}

TypeAST* TypeResolver::resolveResult(ResultTypeAST* result) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveResult");
    
    TypeAST* inner = resolve(result->inner);
    if (!inner) return nullptr;
    
    result->inner = inner;
    
    if (result->errorType) {
        TypeAST* error = resolve(result->errorType);
        if (!error) return nullptr;
        result->errorType = error;
    }
    
    return result;
}

TypeAST* TypeResolver::resolveArray(ArrayTypeAST* array) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveArray: kind=" 
                             << static_cast<int>(array->arrayKind));
    
    TypeAST* element = resolve(array->element);
    if (!element) return nullptr;
    
    array->element = element;
    return array;
}

TypeAST* TypeResolver::resolveRef(RefTypeAST* ref) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveRef");
    
    TypeAST* inner = resolve(ref->inner);
    if (!inner) return nullptr;
    
    ref->inner = inner;
    return ref;
}

TypeAST* TypeResolver::resolvePtr(PtrTypeAST* ptr) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolvePtr");
    
    TypeAST* inner = resolve(ptr->inner);
    if (!inner) return nullptr;
    
    ptr->inner = inner;
    return ptr;
}

TypeAST* TypeResolver::resolveFunc(FuncTypeAST* func) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveFunc");
    
    // Resolve all parameters
    for (auto* param : func->params) {
        if (!resolveParam(param)) {
            return nullptr;
        }
    }
    
    // Resolve all return types
    if (!resolveReturnTypes(func->returnTypes)) {
        return nullptr;
    }
    
    return func;
}

// ============================================================================
// Parameter/Return Resolution
// ============================================================================

bool TypeResolver::resolveParam(ParamAST* param) {
    if (!param->type) {
        ctx_.error(param->loc, DiagCode::E2001, "parameter missing type");
        return false;
    }
    
    TypeAST* resolved = resolve(param->type);
    if (!resolved) return false;
    
    param->valueType = resolved;
    param->type = resolved;  // Update in-place
    
    return true;
}

bool TypeResolver::resolveReturnTypes(ArenaSpan<TypeAST*>& returnTypes) {
    for (size_t i = 0; i < returnTypes.size(); ++i) {
        TypeAST* resolved = resolve(returnTypes[i]);
        if (!resolved) return false;
        returnTypes[i] = resolved;
    }
    return true;
}

// ============================================================================
// Declaration Resolution
// ============================================================================

TypeAST* TypeResolver::resolveVarType(VarDeclAST* var) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveVarType: " 
                             << ctx_.pool.lookup(var->name));
    
    if (!var->type) {
        ctx_.error(var->loc, DiagCode::E2001, "variable missing type");
        return nullptr;
    }
    
    TypeAST* resolved = resolve(var->type);
    if (!resolved) return nullptr;
    
    var->valueType = resolved;
    var->type = resolved;  // Update in-place
    
    return resolved;
}

bool TypeResolver::resolveFunctionSignature(FuncDeclAST* func) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveFunctionSignature: " 
                             << ctx_.pool.lookup(func->name));
    
    if (!func->funcType) {
        ctx_.error(func->loc, DiagCode::E2001, "function missing type signature");
        return false;
    }
    
    // Resolve the function type (parameters and returns)
    FuncTypeAST* resolved = func->funcType->as<FuncTypeAST>();
    if (!resolveFunc(resolved)) {
        return false;
    }
    
    // Cache the first return type for quick access
    if (!resolved->returnTypes.empty()) {
        func->resolvedReturnType = resolved->returnTypes[0];
    }
    
    // Set valueType to the function type
    func->valueType = resolved;
    
    return true;
}

bool TypeResolver::resolveStructFields(StructDeclAST* structDecl) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveStructFields: " 
                             << ctx_.pool.lookup(structDecl->name));
    
    // Ensure selfType exists
    ensureSelfType(structDecl);
    
    // Resolve each field's type
    for (auto* field : structDecl->fields) {
        if (!field->type) {
            ctx_.error(field->loc, DiagCode::E2001,
                       "field '", ctx_.pool.lookup(field->name), "' missing type");
            return false;
        }
        
        TypeAST* resolved = resolve(field->type);
        if (!resolved) return false;
        
        field->valueType = resolved;
        field->type = resolved;  // Update in-place
    }
    
    return true;
}

bool TypeResolver::resolveEnum(EnumDeclAST* enumDecl) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveEnum: " 
                             << ctx_.pool.lookup(enumDecl->name));
    
    // Ensure selfType exists
    ensureSelfType(enumDecl);
    
    // Enums have no nested types to resolve
    return true;
}

bool TypeResolver::resolveTrait(TraitDeclAST* traitDecl) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveTrait: " 
                             << ctx_.pool.lookup(traitDecl->name));
    
    // Ensure selfType exists
    ensureSelfType(traitDecl);
    
    // Resolve trait method signatures
    for (auto* method : traitDecl->methods) {
        if (method->funcType) {
            if (!resolveFunc(method->funcType)) {
                return false;
            }
        }
    }
    
    return true;
}

bool TypeResolver::resolveImpl(ImplDeclAST* impl) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveImpl");
    
    // Resolve the target type
    if (impl->targetType) {
        TypeAST* resolved = resolve(impl->targetType);
        if (!resolved) return false;
        impl->targetType = resolved;
    }
    
    // For generic array target, we don't resolve the type parameter
    // It's a declaration, not a reference
    
    // Resolve method signatures
    for (auto* method : impl->methods) {
        if (method->isInlineBody() && method->funcType) {
            if (!resolveFunc(method->funcType)) {
                return false;
            }
            method->valueType = method->funcType;
        } else if (method->assignmentRef) {
            // Assignment form – type will be resolved during checking
            // For now, we don't resolve the reference
        }
    }
    
    return true;
}

bool TypeResolver::resolveFrom(FromDeclAST* from) {
    LUC_LOG_SEMANTIC_EXTREME("TypeResolver::resolveFrom");
    
    // Resolve the target type
    if (from->targetType) {
        TypeAST* resolved = resolve(from->targetType);
        if (!resolved) return false;
        from->targetType = resolved;
    }
    
    // Resolve each entry
    for (auto* entry : from->entries) {
        if (entry->kind == FromEntryKind::Inline && entry->funcType) {
            if (!resolveFunc(entry->funcType)) {
                return false;
            }
        } else if (entry->kind == FromEntryKind::Path && entry->path) {
            // Path entry – type will be resolved during checking
        }
    }
    
    return true;
}

// ============================================================================
// Type Utilities
// ============================================================================

bool TypeResolver::typesEqual(TypeAST* a, TypeAST* b) {
    // Unwrap aliases
    a = unwrapAlias(a);
    b = unwrapAlias(b);
    
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    
    switch (a->kind) {
        case ASTKind::PrimitiveType: {
            auto* pa = a->as<PrimitiveTypeAST>();
            auto* pb = b->as<PrimitiveTypeAST>();
            return pa->primitiveKind == pb->primitiveKind;
        }
        case ASTKind::NamedType: {
            auto* na = a->as<NamedTypeAST>();
            auto* nb = b->as<NamedTypeAST>();
            if (na->name != nb->name) return false;
            if (na->genericArgs.size() != nb->genericArgs.size()) return false;
            for (size_t i = 0; i < na->genericArgs.size(); ++i) {
                if (!typesEqual(na->genericArgs[i], nb->genericArgs[i])) return false;
            }
            return true;
        }
        case ASTKind::ArrayType: {
            auto* aa = a->as<ArrayTypeAST>();
            auto* ab = b->as<ArrayTypeAST>();
            return aa->arrayKind == ab->arrayKind &&
                   aa->size == ab->size &&
                   typesEqual(aa->element, ab->element);
        }
        case ASTKind::FuncType: {
            auto* fa = a->as<FuncTypeAST>();
            auto* fb = b->as<FuncTypeAST>();
            if (fa->params.size() != fb->params.size()) return false;
            if (fa->returnTypes.size() != fb->returnTypes.size()) return false;
            if (fa->qualifiers != fb->qualifiers) return false;
            for (size_t i = 0; i < fa->params.size(); ++i) {
                if (!typesEqual(fa->params[i]->type, fb->params[i]->type)) return false;
            }
            for (size_t i = 0; i < fa->returnTypes.size(); ++i) {
                if (!typesEqual(fa->returnTypes[i], fb->returnTypes[i])) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

TypeAST* TypeResolver::unwrapAlias(TypeAST* type) {
    if (!type) return nullptr;
    
    while (auto* named = type->as<NamedTypeAST>()) {
        TypeDeclAST* decl = ctx_.scope.lookupType(named->name);
        if (!decl) break;
        
        if (auto* alias = decl->as<TypeAliasDeclAST>()) {
            if (alias->resolvedType) {
                type = alias->resolvedType;
                continue;
            }
        }
        break;
    }
    
    return type;
}

// ============================================================================
// Helpers
// ============================================================================

void TypeResolver::ensureSelfType(TypeDeclAST* typeDecl) {
    if (!typeDecl->selfType) {
        typeDecl->selfType = ctx_.arena.make<NamedTypeAST>(typeDecl->name);
        typeDecl->selfType->loc = typeDecl->loc;
        LUC_LOG_SEMANTIC_EXTREME("TypeResolver: created selfType for " 
                                 << ctx_.pool.lookup(typeDecl->name));
    }
}

} // namespace luc