/**
 * @file TypeCloner.cpp
 * @brief Implementation of type cloning with substitution support.
 */

#include "TypeCloner.hpp"
#include "core/GenericParamHandler.hpp"
#include "debug/DebugMacros.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TypeCloner::TypeCloner(ASTArena& arena, GenericParamHandler& paramHandler)
    : arena_(arena), paramHandler_(paramHandler) {
    LUC_LOG_SEMANTIC("TypeCloner constructed");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Cloning Entry Points
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* TypeCloner::clone(const TypeAST* type) {
    if (!type) return nullptr;
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return clonePrimitive(type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return cloneNamed(type->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return cloneNullable(type->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return cloneResult(type->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return cloneArray(type->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return cloneRef(type->as<RefTypeAST>());
        case ASTKind::PtrType:
            return clonePtr(type->as<PtrTypeAST>());
        case ASTKind::FuncType:
            return cloneFunc(type->as<FuncTypeAST>(), type->loc);
        default:
            LUC_LOG_SEMANTIC("TypeCloner::clone: unhandled kind " << static_cast<int>(type->kind));
            return nullptr;
    }
}

FuncTypeAST* TypeCloner::cloneFunc(const FuncTypeAST* src, const SourceLocation& loc) {
    if (!src) return nullptr;
    auto* dst = arena_.make<FuncTypeAST>().release();
    return cloneFuncInternal(dst, src, loc);
}

TypeAST* TypeCloner::cloneWithSubstitution(const TypeAST* type) {
    if (!type) return nullptr;
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return clonePrimitive(type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return cloneNamedWithSubstitution(type->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return cloneNullable(type->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return cloneResult(type->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return cloneArray(type->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return cloneRef(type->as<RefTypeAST>());
        case ASTKind::PtrType:
            return clonePtr(type->as<PtrTypeAST>());
        case ASTKind::FuncType: {
            auto* dst = arena_.make<FuncTypeAST>().release();
            return cloneFuncWithSubstitutionInternal(dst, type->as<FuncTypeAST>(), type->loc);
        }
        default:
            LUC_LOG_SEMANTIC("TypeCloner::cloneWithSubstitution: unhandled kind");
            return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Type-Specific Clone Helpers
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* TypeCloner::clonePrimitive(const PrimitiveTypeAST* src) {
    if (!src) return nullptr;
    return arena_.make<PrimitiveTypeAST>(src->primitiveKind).release();
}

TypeAST* TypeCloner::cloneNamed(const NamedTypeAST* src) {
    if (!src) return nullptr;
    
    auto* dst = arena_.make<NamedTypeAST>(src->name).release();
    dst->loc = src->loc;
    dst->isGenericParam = src->isGenericParam;
    
    // Clone generic arguments
    if (!src->genericArgs.empty()) {
        auto builder = arena_.makeBuilder<TypePtr>();
        for (const auto& arg : src->genericArgs) {
            builder.push_back(TypePtr(clone(arg.get())));
        }
        dst->genericArgs = builder.build();
    }
    
    return dst;
}

TypeAST* TypeCloner::cloneNamedWithSubstitution(const NamedTypeAST* src) {
    if (!src) return nullptr;
    
    // If this is a generic parameter, check for a substitution
    if (src->isGenericParam) {
        TypeAST* subst = paramHandler_.lookupSubst(src->name);
        if (subst) {
            // Found a substitution - return a clone of the substituted type
            // (don't clone as NamedTypeAST, clone the actual substituted type)
            return cloneWithSubstitution(subst);
        }
        // No substitution - keep as generic parameter
        return cloneNamed(src);
    }
    
    // Not a generic parameter - normal clone
    return cloneNamed(src);
}

TypeAST* TypeCloner::cloneNullable(const NullableTypeAST* src) {
    if (!src) return nullptr;
    return arena_.make<NullableTypeAST>(
        TypePtr(clone(src->inner.get()))
    ).release();
}

TypeAST* TypeCloner::cloneResult(const ResultTypeAST* src) {
    if (!src) return nullptr;
    return arena_.make<ResultTypeAST>(
        TypePtr(clone(src->inner.get())),
        TypePtr(src->errorType ? clone(src->errorType.get()) : nullptr)
    ).release();
}

TypeAST* TypeCloner::cloneArray(const ArrayTypeAST* src) {
    if (!src) return nullptr;
    return arena_.make<ArrayTypeAST>(
        src->arrayKind,
        src->size,
        TypePtr(clone(src->element.get()))
    ).release();
}

TypeAST* TypeCloner::cloneRef(const RefTypeAST* src) {
    if (!src) return nullptr;
    return arena_.make<RefTypeAST>(
        TypePtr(clone(src->inner.get()))
    ).release();
}

TypeAST* TypeCloner::clonePtr(const PtrTypeAST* src) {
    if (!src) return nullptr;
    return arena_.make<PtrTypeAST>(
        TypePtr(clone(src->inner.get()))
    ).release();
}

// ─────────────────────────────────────────────────────────────────────────────
// Function Type Cloning Helpers
// ─────────────────────────────────────────────────────────────────────────────

FuncTypeAST* TypeCloner::cloneFuncInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc) {
    if (!src || !dst) return nullptr;
    
    // Copy qualifiers and raw qualifiers
    dst->qualifiers = src->qualifiers;
    
    if (!src->rawQualifiers.empty()) {
        auto qBuilder = arena_.makeBuilder<InternedString>();
        for (const auto& q : src->rawQualifiers) {
            qBuilder.push_back(q);
        }
        dst->rawQualifiers = qBuilder.build();
    }
    
    // Clone parameters (flattened)
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    for (size_t g = 0; g < src->sig.groupCount(); ++g) {
        auto group = src->sig.getGroup(g);
        groupSizes.push_back(group.size());
        for (const auto& param : group) {
            auto* newParam = arena_.make<ParamAST>().release();
            newParam->name = param->name;
            newParam->type = TypePtr(clone(param->type.get()));
            newParam->isVariadic = param->isVariadic;
            newParam->loc = param->loc;
            allParams.emplace_back(newParam);
        }
    }
    
    auto paramBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    dst->sig.allParams = paramBuilder.build();
    
    auto sizeBuilder = arena_.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    dst->sig.groupSizes = sizeBuilder.build();
    
    // Clone return types
    auto retBuilder = arena_.makeBuilder<TypePtr>();
    for (const auto& ret : src->sig.returnTypes) {
        retBuilder.push_back(TypePtr(clone(ret.get())));
    }
    dst->sig.returnTypes = retBuilder.build();
    
    dst->loc = loc;
    return dst;
}

FuncTypeAST* TypeCloner::cloneFuncWithSubstitutionInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc) {
    if (!src || !dst) return nullptr;
    
    // Copy qualifiers and raw qualifiers
    dst->qualifiers = src->qualifiers;
    
    if (!src->rawQualifiers.empty()) {
        auto qBuilder = arena_.makeBuilder<InternedString>();
        for (const auto& q : src->rawQualifiers) {
            qBuilder.push_back(q);
        }
        dst->rawQualifiers = qBuilder.build();
    }
    
    // Clone parameters with substitution
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    for (size_t g = 0; g < src->sig.groupCount(); ++g) {
        auto group = src->sig.getGroup(g);
        groupSizes.push_back(group.size());
        for (const auto& param : group) {
            auto* newParam = arena_.make<ParamAST>().release();
            newParam->name = param->name;
            newParam->type = TypePtr(cloneWithSubstitution(param->type.get()));
            newParam->isVariadic = param->isVariadic;
            newParam->loc = param->loc;
            allParams.emplace_back(newParam);
        }
    }
    
    auto paramBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    dst->sig.allParams = paramBuilder.build();
    
    auto sizeBuilder = arena_.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    dst->sig.groupSizes = sizeBuilder.build();
    
    // Clone return types with substitution
    auto retBuilder = arena_.makeBuilder<TypePtr>();
    for (const auto& ret : src->sig.returnTypes) {
        retBuilder.push_back(TypePtr(cloneWithSubstitution(ret.get())));
    }
    dst->sig.returnTypes = retBuilder.build();
    
    dst->loc = loc;
    return dst;
}