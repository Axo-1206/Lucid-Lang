/**
 * @file FromResolver.cpp
 * @brief Implementation of from resolution.
 */

#include "FromResolver.hpp"
#include "../core/TypeCloner.hpp"
#include "debug/DebugMacros.hpp"

FromResolver::FromResolver(SemanticContext& ctx,
                           GenericParamHandler& paramHandler,
                           CallableExtractor& callableExtractor)
    : ctx_(ctx), paramHandler_(paramHandler), callableExtractor_(callableExtractor) {
    LUC_LOG_SEMANTIC("FromResolver constructed");
}

void FromResolver::resolve(FromDeclAST& node) {
    LUC_LOG_SEMANTIC("FromResolver::resolve");
    TypeAST* targetType = node.targetType.get();
    if (!targetType) {
        ctx_.error(node.loc, DiagCode::E2001, "cannot resolve from target type");
        return;
    }

    paramHandler_.pushParams(&node.genericParams);
    for (auto& entry : node.entries) {
        if (!entry) continue;
        if (entry->path) {
            resolvePathEntry(*entry, targetType);
        } else {
            resolveInlineEntry(*entry, targetType);
        }
    }
    paramHandler_.popParams();
}

void FromResolver::resolveInlineEntry(FromEntryAST& entry, TypeAST* targetType) {
    auto* funcType = TypeCloner::createFuncType(ctx_.arena, entry.loc);

    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    for (size_t g = 0; g < entry.sig.groupCount(); ++g) {
        auto group = entry.sig.getGroup(g);
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
    funcType->sig.allParams = paramBuilder.build();

    auto sizeBuilder = ctx_.arena.makeBuilder<size_t>();
    for (auto sz : groupSizes) sizeBuilder.push_back(sz);
    funcType->sig.groupSizes = sizeBuilder.build();

    auto retBuilder = ctx_.arena.makeBuilder<TypePtr>();
    retBuilder.push_back(TypePtr(TypeCloner::clone(ctx_.arena, entry.returnType.get())));
    funcType->sig.returnTypes = retBuilder.build();

    if (!validateReturnType(entry, targetType)) {
        ctx_.error(entry.loc, DiagCode::E2019, "from entry return type does not match target type");
    }
}

void FromResolver::resolvePathEntry(FromEntryAST& entry, TypeAST* targetType) {
    ArenaSpan<TypePtr> emptyTypeArgs;
    TypeAST* resolved = callableExtractor_.resolveReference(entry.path, emptyTypeArgs, entry.loc);
    if (!resolved || !resolved->isa<FuncTypeAST>()) {
        ctx_.error(entry.loc, DiagCode::E2002, "path entry must resolve to a function");
        return;
    }
    auto* funcType = resolved->as<FuncTypeAST>();
    if (funcType->sig.returnTypes.empty()) {
        ctx_.error(entry.loc, DiagCode::E2002, "path entry function must have a return type");
        return;
    }
    // Return type check deferred to Phase 3.
}

bool FromResolver::validateReturnType(FromEntryAST& entry, TypeAST* targetType) {
    // Placeholder; full type equality will be checked in Phase 3.
    return true;
}