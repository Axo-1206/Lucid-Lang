/**
 * @file TypeCloner.cpp
 * @brief Implementation of type cloning with substitution support.
 */

#include "TypeCloner.hpp"
#include "GenericParamHandler.hpp"
#include "debug/DebugMacros.hpp"

namespace TypeCloner {

// ─────────────────────────────────────────────────────────────────────────────
// Private Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

TypeAST* clonePrimitive(ASTArena& arena, const PrimitiveTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<PrimitiveTypeAST>(src->primitiveKind).release();
    dst->loc = src->loc;
    return dst;
}

TypeAST* cloneNamed(ASTArena& arena, const NamedTypeAST* src) {
    if (!src) return nullptr;
    
    auto* dst = arena.make<NamedTypeAST>(src->name).release();
    dst->loc = src->loc;
    dst->isGenericParam = src->isGenericParam;
    
    if (!src->genericArgs.empty()) {
        auto builder = arena.makeBuilder<TypePtr>();
        for (const auto& arg : src->genericArgs) {
            builder.push_back(TypePtr(clone(arena, arg.get())));
        }
        dst->genericArgs = builder.build();
    }
    
    return dst;
}

TypeAST* cloneNamedWithSubstitution(ASTArena& arena, GenericParamHandler& paramHandler, const NamedTypeAST* src) {
    if (!src) return nullptr;
    
    // If this is a generic parameter, check for a substitution
    if (src->isGenericParam) {
        TypeAST* subst = paramHandler.lookupSubst(src->name);
        if (subst) {
            // Found a substitution - return a clone of the substituted type
            return cloneWithSubstitution(arena, paramHandler, subst);
        }
        // No substitution - keep as generic parameter
    }
    
    // Not a generic parameter (or no substitution) - normal clone
    return cloneNamed(arena, src);
}

TypeAST* cloneNullable(ASTArena& arena, const NullableTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<NullableTypeAST>(
        TypePtr(clone(arena, src->inner.get()))
    ).release();
    dst->loc = src->loc;
    return dst;
}

TypeAST* cloneResult(ASTArena& arena, const ResultTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<ResultTypeAST>(
        TypePtr(clone(arena, src->inner.get())),
        TypePtr(src->errorType ? clone(arena, src->errorType.get()) : nullptr)
    ).release();
    dst->loc = src->loc;
    return dst;
}

TypeAST* cloneArray(ASTArena& arena, const ArrayTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<ArrayTypeAST>(
        src->arrayKind,
        src->size,
        TypePtr(clone(arena, src->element.get()))
    ).release();
    dst->loc = src->loc;
    return dst;
}

TypeAST* cloneRef(ASTArena& arena, const RefTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<RefTypeAST>(
        TypePtr(clone(arena, src->inner.get()))
    ).release();
    dst->loc = src->loc;
    return dst;
}

TypeAST* clonePtr(ASTArena& arena, const PtrTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<PtrTypeAST>(
        TypePtr(clone(arena, src->inner.get()))
    ).release();
    dst->loc = src->loc;
    return dst;
}

FuncTypeAST* cloneFuncInternal(ASTArena& arena, FuncTypeAST* dst, const FuncTypeAST* src) {
    if (!src || !dst) return nullptr;
    
    // Copy qualifiers
    dst->qualifiers = src->qualifiers;
    dst->loc = src->loc;
    
    // Copy raw qualifiers
    if (!src->rawQualifiers.empty()) {
        auto qBuilder = arena.makeBuilder<InternedString>();
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
            auto* newParam = arena.make<ParamAST>().release();
            newParam->name = param->name;
            newParam->type = TypePtr(clone(arena, param->type.get()));
            newParam->isVariadic = param->isVariadic;
            newParam->loc = param->loc;
            allParams.emplace_back(newParam);
        }
    }
    
    auto paramBuilder = arena.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    dst->sig.allParams = paramBuilder.build();
    
    auto sizeBuilder = arena.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    dst->sig.groupSizes = sizeBuilder.build();
    
    // Clone return types
    auto retBuilder = arena.makeBuilder<TypePtr>();
    for (const auto& ret : src->sig.returnTypes) {
        retBuilder.push_back(TypePtr(clone(arena, ret.get())));
    }
    dst->sig.returnTypes = retBuilder.build();
    
    return dst;
}

FuncTypeAST* cloneFuncWithSubstitutionInternal(ASTArena& arena, GenericParamHandler& paramHandler,
                                                FuncTypeAST* dst, const FuncTypeAST* src) {
    if (!src || !dst) return nullptr;
    
    // Copy qualifiers
    dst->qualifiers = src->qualifiers;
    dst->loc = src->loc;
    
    // Copy raw qualifiers
    if (!src->rawQualifiers.empty()) {
        auto qBuilder = arena.makeBuilder<InternedString>();
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
            auto* newParam = arena.make<ParamAST>().release();
            newParam->name = param->name;
            newParam->type = TypePtr(cloneWithSubstitution(arena, paramHandler, param->type.get()));
            newParam->isVariadic = param->isVariadic;
            newParam->loc = param->loc;
            allParams.emplace_back(newParam);
        }
    }
    
    auto paramBuilder = arena.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    dst->sig.allParams = paramBuilder.build();
    
    auto sizeBuilder = arena.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    dst->sig.groupSizes = sizeBuilder.build();
    
    // Clone return types with substitution
    auto retBuilder = arena.makeBuilder<TypePtr>();
    for (const auto& ret : src->sig.returnTypes) {
        retBuilder.push_back(TypePtr(cloneWithSubstitution(arena, paramHandler, ret.get())));
    }
    dst->sig.returnTypes = retBuilder.build();
    
    return dst;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

TypeAST* clone(ASTArena& arena, const TypeAST* type) {
    if (!type) return nullptr;
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return clonePrimitive(arena, type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return cloneNamed(arena, type->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return cloneNullable(arena, type->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return cloneResult(arena, type->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return cloneArray(arena, type->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return cloneRef(arena, type->as<RefTypeAST>());
        case ASTKind::PtrType:
            return clonePtr(arena, type->as<PtrTypeAST>());
        case ASTKind::FuncType:
            return cloneFunc(arena, type->as<FuncTypeAST>());
        default:
            LUC_LOG_SEMANTIC("TypeCloner::clone: unhandled kind " << static_cast<int>(type->kind));
            return nullptr;
    }
}

FuncTypeAST* cloneFunc(ASTArena& arena, const FuncTypeAST* src) {
    if (!src) return nullptr;
    auto* dst = arena.make<FuncTypeAST>().release();
    return cloneFuncInternal(arena, dst, src);
}

TypeAST* cloneWithSubstitution(ASTArena& arena, GenericParamHandler& paramHandler, const TypeAST* type) {
    if (!type) return nullptr;
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return clonePrimitive(arena, type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return cloneNamedWithSubstitution(arena, paramHandler, type->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return cloneNullable(arena, type->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return cloneResult(arena, type->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return cloneArray(arena, type->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return cloneRef(arena, type->as<RefTypeAST>());
        case ASTKind::PtrType:
            return clonePtr(arena, type->as<PtrTypeAST>());
        case ASTKind::FuncType: {
            auto* dst = arena.make<FuncTypeAST>().release();
            return cloneFuncWithSubstitutionInternal(arena, paramHandler, dst, type->as<FuncTypeAST>());
        }
        default:
            LUC_LOG_SEMANTIC("TypeCloner::cloneWithSubstitution: unhandled kind");
            return nullptr;
    }
}

FuncTypeAST* createFuncType(ASTArena& arena, const SourceLocation& loc) {
    auto* dst = arena.make<FuncTypeAST>().release();
    dst->loc = loc;
    return dst;
}

} // namespace TypeCloner