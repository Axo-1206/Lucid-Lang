/// @file sema/types/GenericHelpers.cpp
/// @brief Implementation of generic type checking helpers.

#include "GenericHelpers.hpp"
namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameter Detection
// ─────────────────────────────────────────────────────────────────────────────

bool isGenericParameterType(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        
        // Check if resolvedDecl is a GenericParamDeclAST
        if (named->resolvedDecl && named->resolvedDecl->isa<GenericParamDeclAST>()) {
            return true;
        }
        
        // Fallback: check if the name matches a generic param in the current scope
        if (ctx.isGenericParam(named->name)) {
            return true;
        }
    }
    
    return false;
}

bool containsGenericParameter(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    
    switch (type->kind) {
        case ASTKind::NamedType: {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            if (isGenericParameterType(named, ctx)) return true;
            for (TypeAST* arg : named->genericArgs) {
                if (containsGenericParameter(arg, ctx)) return true;
            }
            return false;
        }
        
        case ASTKind::ArrayType: {
            ArrayTypeAST* arr = type->as<ArrayTypeAST>();
            return containsGenericParameter(arr->element, ctx);
        }
        
        case ASTKind::NullableType: {
            NullableTypeAST* nullable = type->as<NullableTypeAST>();
            return containsGenericParameter(nullable->inner, ctx);
        }
        
        case ASTKind::FallibleType: {
            FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
            return containsGenericParameter(fallible->inner, ctx);
        }
        
        case ASTKind::CombinedType: {
            CombinedTypeAST* combined = type->as<CombinedTypeAST>();
            return containsGenericParameter(combined->inner, ctx);
        }
        
        case ASTKind::RefType: {
            RefTypeAST* ref = type->as<RefTypeAST>();
            return containsGenericParameter(ref->inner, ctx);
        }
        
        case ASTKind::PtrType: {
            PtrTypeAST* ptr = type->as<PtrTypeAST>();
            return containsGenericParameter(ptr->inner, ctx);
        }
        
        case ASTKind::FuncType: {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            for (ParamAST* param : func->params) {
                if (containsGenericParameter(param->type, ctx)) return true;
            }
            if (func->returnType && containsGenericParameter(func->returnType, ctx)) {
                return true;
            }
            return false;
        }
        
        case ASTKind::FutureType: {
            FutureTypeAST* future = type->as<FutureTypeAST>();
            return containsGenericParameter(future->inner, ctx);
        }
        
        case ASTKind::ThreadType: {
            ThreadTypeAST* thread = type->as<ThreadTypeAST>();
            return containsGenericParameter(thread->inner, ctx);
        }
        
        case ASTKind::SimdType: {
            SimdTypeAST* simd = type->as<SimdTypeAST>();
            return containsGenericParameter(simd->elementType, ctx);
        }
        
        default:
            return false;
    }
}

bool isConcreteType(TypeAST* type, SemaContext& ctx) {
    if (!type) return false;
    if (isGenericParameterType(type, ctx)) return false;
    if (containsGenericParameter(type, ctx)) return false;
    return true;
}

} // namespace sema