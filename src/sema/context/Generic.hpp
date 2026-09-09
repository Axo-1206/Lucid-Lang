/// @file sema/context/Generic.hpp
/// @brief Generic instantiation and substitution utilities for Sema.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "core/memory/InternedString.hpp"
#include "sema/context/SemaContext.hpp"

#include <unordered_set>
#include <functional>

namespace sema {

// ─── GenericSubstitution ─────────────────────────────────────────────────────

/// @brief Simple substitution context for generic parameters to concrete types.
struct GenericSubstitution {
    const ArenaSpan<GenericParamDeclAST*>& genericParams;
    const ArenaSpan<TypeAST*>& typeArgs;

    GenericSubstitution(
        const ArenaSpan<GenericParamDeclAST*>& params,
        const ArenaSpan<TypeAST*>& args)
        : genericParams(params), typeArgs(args) {}

    TypeAST* lookup(InternedString name) const {
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name && i < typeArgs.size()) {
                return typeArgs[i];
            }
        }
        return nullptr;
    }

    bool isParam(InternedString name) const {
        for (auto p : genericParams) {
            if (p->name == name) return true;
        }
        return false;
    }

    size_t paramCount() const { return genericParams.size(); }
    size_t argCount() const { return typeArgs.size(); }
    bool isComplete() const { return typeArgs.size() == genericParams.size(); }
};

// ─── GenericResolution ─────────────────────────────────────────────────────

/// @brief Result of resolving a generic instantiation.
/// 
/// Contains either:
///   - A specialized declaration (default path) with genericParams empty
///   - The template declaration with an erased name (type-erased path, @[erased])
struct GenericResolution {
    /// The resolved declaration (specialized or template).
    DeclAST* resolvedDecl = nullptr;
    
    /// True if this resolved to a type-erased declaration (@[erased] path).
    bool isErased = false;
};

// ─── Type Substitution Helpers (declarations) ──────────────────────────────

/// @brief Substitute generic parameters in a type.
TypeAST* substituteType(TypeAST* type, const GenericSubstitution& subst, SemaContext& ctx);

/// @brief Substitute generic parameters in a statement.
StmtAST* substituteStmt(StmtAST* stmt, const GenericSubstitution& subst, SemaContext& ctx);

/// @brief Substitute generic parameters in an expression.
ExprAST* substituteExpr(ExprAST* expr, const GenericSubstitution& subst, SemaContext& ctx);

/// @brief Check if a type contains any generic parameters.
bool containsGenericParams(TypeAST* type, const GenericSubstitution& subst);

// ─── Generic Resolution (declaration) ─────────────────────────────────────

/// @brief Resolve a generic instantiation, choosing between specialization and type-erasure.
/// 
/// This is the SINGLE decision point for generic instantiation. It handles:
///   - Arity validation
///   - Specialized (default) vs type-erased (@[erased]) path selection
///   - Specialized declaration creation (if needed)
/// 
/// @param templateDecl The generic declaration (FuncDeclAST* or StructDeclAST*).
/// @param typeArgs The concrete type arguments provided at the use site.
/// @param ctx The semantic context.
/// @return A GenericResolution containing the resolved declaration and strategy.
/// 
/// @note This function should be called ONCE per instantiation. Call sites
///       should store the result on the AST node (e.g., resolvedDecl, isErased)
///       rather than re-deriving it.
GenericResolution resolveGenericInstantiation(
    DeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx);

// ─── Instantiated Struct/Function Creation (declarations) ──────────────────

/// @brief Create an instantiated struct from a generic template.
/// 
/// This creates a specialized struct (default path) where all generic
/// parameters are substituted with concrete types.
/// 
/// @param templateDecl The generic struct template.
/// @param typeArgs The concrete type arguments.
/// @param ctx The semantic context.
/// @return The instantiated StructDeclAST, or nullptr on error.
StructDeclAST* createInstantiatedStruct(
    StructDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx);

/// @brief Create an instantiated function from a generic template.
/// 
/// This creates a specialized function (default path) where all generic
/// parameters are substituted with concrete types.
/// 
/// @param templateDecl The generic function template.
/// @param typeArgs The concrete type arguments.
/// @param ctx The semantic context.
/// @return The instantiated FuncDeclAST, or nullptr on error.
FuncDeclAST* createInstantiatedFunction(
    FuncDeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx);

} // namespace sema