/**
 * @file InjectionTransformer.cpp
 * @brief Implementation of injection transformation.
 */

#include "InjectionTransformer.hpp"
#include "../core/TypeCloner.hpp"
#include "debug/DebugMacros.hpp"

InjectionTransformer::InjectionTransformer(SemanticContext& ctx)
    : ctx_(ctx) {
    LUC_LOG_SEMANTIC("InjectionTransformer constructed");
}

FuncTypeAST* InjectionTransformer::transform(FuncTypeAST* funcType,
                                             InternedString /*receiverName*/,
                                             const SourceLocation& loc) {
    if (!funcType) {
        ctx_.error(loc, DiagCode::E2002, "cannot transform null function type for injection");
        return nullptr;
    }
    if (!validateTransformable(funcType, loc)) {
        return nullptr;
    }
    return createTransformedType(funcType, loc);
}

bool InjectionTransformer::validateTransformable(const FuncTypeAST* funcType, const SourceLocation& loc) {
    if (funcType->sig.groupCount() == 0) {
        ctx_.error(loc, DiagCode::E2002, "injection requires a function with at least one parameter");
        return false;
    }
    auto firstGroup = funcType->sig.getGroup(0);
    if (firstGroup.empty()) {
        ctx_.error(loc, DiagCode::E2002, "injection requires the first parameter group to have at least one parameter");
        return false;
    }
    return true;
}

FuncTypeAST* InjectionTransformer::createTransformedType(const FuncTypeAST* src, const SourceLocation& loc) {
    auto* dst = TypeCloner::createFuncType(ctx_.arena, loc);
    dst->qualifiers = src->qualifiers;
    // Copy raw qualifiers
    if (!src->rawQualifiers.empty()) {
        auto qBuilder = ctx_.arena.makeBuilder<InternedString>();
        for (const auto& q : src->rawQualifiers) qBuilder.push_back(q);
        dst->rawQualifiers = qBuilder.build();
    }

    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;

    // Skip first parameter of first group
    auto firstGroup = src->sig.getGroup(0);
    if (firstGroup.size() > 1) {
        groupSizes.push_back(firstGroup.size() - 1);
        for (size_t i = 1; i < firstGroup.size(); ++i) {
            auto* param = ctx_.arena.make<ParamAST>().release();
            param->name = firstGroup[i]->name;
            param->type = TypePtr(TypeCloner::clone(ctx_.arena, firstGroup[i]->type.get()));
            param->isVariadic = firstGroup[i]->isVariadic;
            param->loc = firstGroup[i]->loc;
            allParams.emplace_back(param);
        }
    }

    // Copy remaining groups unchanged
    for (size_t g = 1; g < src->sig.groupCount(); ++g) {
        auto group = src->sig.getGroup(g);
        groupSizes.push_back(group.size());
        for (const auto& param : group) {
            auto* newParam = ctx_.arena.make<ParamAST>().release();
            newParam->name = param->name;
            newParam->type = TypePtr(TypeCloner::clone(ctx_.arena, param->type.get()));
            newParam->isVariadic = param->isVariadic;
            newParam->loc = param->loc;
            allParams.emplace_back(newParam);
        }
    }

    auto paramBuilder = ctx_.arena.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramBuilder.push_back(std::move(p));
    dst->sig.allParams = paramBuilder.build();

    auto sizeBuilder = ctx_.arena.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    dst->sig.groupSizes = sizeBuilder.build();

    // Clone return types
    auto retBuilder = ctx_.arena.makeBuilder<TypePtr>();
    for (const auto& ret : src->sig.returnTypes) {
        retBuilder.push_back(TypePtr(TypeCloner::clone(ctx_.arena, ret.get())));
    }
    dst->sig.returnTypes = retBuilder.build();

    return dst;
}