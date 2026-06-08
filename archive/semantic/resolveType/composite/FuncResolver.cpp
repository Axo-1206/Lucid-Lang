/**
 * @file FuncResolver.cpp
 * @brief Implementation of function type resolution.
 */

#include "FuncResolver.hpp"
#include "../core/GenericParamHandler.hpp"
#include "../core/TypeCloner.hpp"
#include "registry/QualifierRegistry.hpp"
#include "debug/DebugMacros.hpp"

FuncResolver::FuncResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
    : ctx_(ctx), paramHandler_(paramHandler) {
    LUC_LOG_SEMANTIC("FuncResolver constructed");
}

TypeAST* FuncResolver::resolve(FuncTypeAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("FuncResolver::resolve");
    resolveQualifiers(node);
    // Parameters and return types are resolved by the caller (TypeDispatcher)
    return &node;
}

void FuncResolver::resolveQualifiers(FuncTypeAST& node) {
    for (const auto& qualName : node.rawQualifiers) {
        const QualifierEntry* entry = qualifier::lookup(qualName);
        if (!entry) {
            ctx_.error(node.loc, DiagCode::E1016, "~", ctx_.pool.lookup(qualName));
        } else {
            if (node.qualifiers & entry->bit) {
                ctx_.error(node.loc, DiagCode::E1018, "duplicate qualifier");
            } else {
                node.qualifiers |= entry->bit;
            }
        }
    }
}

void FuncResolver::resolveParameters(FuncTypeAST& node) {
    for (auto& param : node.sig.allParams) {
        if (param && param->type) {
            // Parameter types will be resolved by caller
            LUC_LOG_SEMANTIC_EXTREME("FuncResolver: parameter " << ctx_.pool.lookup(param->name));
        }
    }
}

void FuncResolver::resolveReturnTypes(FuncTypeAST& node) {
    for (auto& retType : node.sig.returnTypes) {
        if (retType) {
            // Return types will be resolved by caller
            LUC_LOG_SEMANTIC_EXTREME("FuncResolver: return type");
        }
    }
}

TypeAST* FuncResolver::getReturnType(const FuncTypeAST& type, const SourceLocation* loc) {
    if (type.sig.returnTypes.empty()) return nullptr;
    if (type.sig.returnTypes.size() > 1) {
        if (loc) {
            ctx_.error(*loc, DiagCode::E2002,
                       "function has multiple return types but single return expected");
        }
        return nullptr;
    }
    return type.sig.returnTypes[0].get();
}

std::vector<TypeAST*> FuncResolver::getReturnTypes(const FuncTypeAST& type) {
    std::vector<TypeAST*> result;
    for (auto& retType : type.sig.returnTypes) {
        if (retType) result.push_back(retType.get());
    }
    return result;
}