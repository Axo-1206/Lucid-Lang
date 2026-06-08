/**
 * @file InjectionTransformer.hpp
 * @brief Transforms function types for injection form (!).
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class InjectionTransformer {
public:
    explicit InjectionTransformer(SemanticContext& ctx);

    FuncTypeAST* transform(FuncTypeAST* funcType,
                           InternedString receiverName,
                           const SourceLocation& loc);

private:
    bool validateTransformable(const FuncTypeAST* funcType, const SourceLocation& loc);
    FuncTypeAST* createTransformedType(const FuncTypeAST* src, const SourceLocation& loc);

    SemanticContext& ctx_;
};