/**
 * @file TypeChecker.hpp
 * @brief Type compatibility utilities as a pure namespace.
 *
 * All functions are stateless and operate only on the provided SemanticContext.
 */

#pragma once

#include "semantic/SemanticSymbol.hpp"
#include "ast/TypeAST.hpp"
#include "ast/ExprAST.hpp"
#include "diagnostics/Diagnostic.hpp"

struct SemanticContext;  // forward declaration

namespace TypeChecker {

    // ── Type compatibility ───────────────────────────────────────────────────
    bool isEqual(TypeAST* a, TypeAST* b, SemanticContext& ctx);
    bool isAssignable(TypeAST* from, TypeAST* to, SemanticContext& ctx);
    bool areAssignableMultiple(const std::vector<TypeAST*>& fromTypes,
                               const std::vector<TypeAST*>& toTypes,
                               SemanticContext& ctx);
    bool isCallable(TypeAST* type, SemanticContext& ctx);
    bool isBooleanCompatible(TypeAST* type, SemanticContext& ctx);
    bool isNullable(TypeAST* type, SemanticContext& ctx);
    TypeAST* unify(TypeAST* a, TypeAST* b, SemanticContext& ctx);
    bool primitiveWidening(PrimitiveKind from, PrimitiveKind to);

    // ── Custom conversion lookup ───────────────────────────────────────────
    Symbol* isFromCastable(TypeAST* src, TypeAST* target, SemanticContext& ctx);
    Symbol* isFromCastableMulti(const std::vector<TypeAST*>& srcTypes, TypeAST* target, SemanticContext& ctx);

    // ── Type queries ───────────────────────────────────────────────────────
    bool isIntegerType(TypeAST* type, SemanticContext& ctx);
    bool isValidArrayIndex(ExprAST* indexExpr, const SourceLocation& loc, SemanticContext& ctx);
    bool isValidSliceBound(ExprAST* boundExpr, const std::string& boundName,
                           const SourceLocation& loc, SemanticContext& ctx);
    bool getConstantIntValue(ExprAST* expr, int64_t& outValue, SemanticContext& ctx);

    // ── Comparison helpers ─────────────────────────────────────────────────
    bool isValueComparable(TypeAST* type, SemanticContext& ctx);
    bool isReferenceComparable(TypeAST* type, SemanticContext& ctx);
    bool isBoolOrNullable(TypeAST* type, SemanticContext& ctx);

} // namespace TypeChecker