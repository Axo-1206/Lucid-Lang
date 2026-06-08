/**
 * @file FromResolver.hpp
 * @brief Resolves from declarations (implicit conversions).
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "../core/GenericParamHandler.hpp"
#include "../callable/CallableExtractor.hpp"

class FromResolver {
public:
    FromResolver(SemanticContext& ctx,
                 GenericParamHandler& paramHandler,
                 CallableExtractor& callableExtractor);

    void resolve(FromDeclAST& node);

private:
    void resolveInlineEntry(FromEntryAST& entry, TypeAST* targetType);
    void resolvePathEntry(FromEntryAST& entry, TypeAST* targetType);
    bool validateReturnType(FromEntryAST& entry, TypeAST* targetType);

    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    CallableExtractor& callableExtractor_;
};