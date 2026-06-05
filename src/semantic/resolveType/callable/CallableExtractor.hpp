/**
 * @file CallableExtractor.hpp
 * @brief Extracts function types from callable expressions.
 */

#pragma once

#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class CallableExtractor {
public:
    explicit CallableExtractor(SemanticContext& ctx);

    FuncTypeAST* extract(ExprPtr& callable,
                         ArenaSpan<TypePtr>& explicitTypeArgs,
                         const SourceLocation& loc);

    TypeAST* resolveReference(ExprPtr& ref,
                              ArenaSpan<TypePtr>& typeArgs,
                              const SourceLocation& loc);

private:
    FuncTypeAST* extractFromIdentifier(IdentifierExprAST* ident, const SourceLocation& loc);
    FuncTypeAST* extractFromFieldAccess(FieldAccessExprAST* field, const SourceLocation& loc);
    FuncTypeAST* extractFromCallableRef(CallableRefExprAST* callableRef, const SourceLocation& loc);
    FuncTypeAST* extractFromBehaviorAccess(BehaviorAccessExprAST* behavior, const SourceLocation& loc);

    SemanticContext& ctx_;
};