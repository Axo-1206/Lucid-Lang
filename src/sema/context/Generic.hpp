/// @file sema/context/Generic.hpp
/// @brief Generic substitution and instantiation utilities.
///
/// ─── Two Concerns, One Header ─────────────────────────────────────────────
/// This header declares two related but distinct services:
///
///   1. **Substitution** — a mechanical tree-rewriting pass. Given a
///      GenericSubstitution (a mapping from generic parameters to concrete
///      types), it produces a copy of the input AST with every occurrence
///      of a generic parameter replaced by its concrete type. Implemented
///      in Generic.cpp.
///
///   2. **Instantiation** — an orchestration pass. It drives substitution,
///      manages the instantiation cache, validates arity, and produces the
///      final specialized declaration. Implemented in Instantiation.cpp.
///
/// The split keeps the "pure transformation" logic separate from the
/// "register, validate, cache" logic. Substitution carries its own
/// SubstitutionContext (see below) rather than polluting SemaContext with
/// pass-scoped state; instantiation leans on SemaContext's
/// instantiationCache, which is a service, not pass state.

#pragma once

#include "SemaContext.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"

namespace sema {

// ─── GenericSubstitution ──────────────────────────────────────────────

/// @brief The mapping from generic parameters to concrete types for one
///        instantiation.
///
/// The two spans are parallel: `genericParams[i]` is substituted by
/// `typeArgs[i]`. Constructed once per instantiation and passed by const
/// reference through the whole substitution walk.
struct GenericSubstitution {
    ArenaSpan<GenericParamDeclAST*> genericParams;
    ArenaSpan<TypeAST*> typeArgs;

    GenericSubstitution(ArenaSpan<GenericParamDeclAST*> params,
                        ArenaSpan<TypeAST*> args)
        : genericParams(params), typeArgs(args) {}

    bool isParam(InternedString name) const;
    TypeAST* lookup(InternedString name) const;
};

// ─── SubstitutionContext ──────────────────────────────────────────────

/// @brief Everything substitution needs beyond the substitution map.
///
/// Passed by reference through the substitution recursion. Keeps
/// substitution-scoped state off SemaContext, which is shared across
/// all passes and shouldn't accumulate per-pass fields.
///
/// As substitution grows (partial substitution, nested-generic scopes,
/// substitution-aware diagnostics), this struct is where new state goes.
struct SubstitutionContext {
    /// The semantic context — used for arena allocation, diagnostics,
    /// the type cache (getArrayType etc.), and other shared services.
    /// Substitution never mutates any SemaContext field; it reads.
    SemaContext& sema;

    /// The substitution map for this instantiation.
    const GenericSubstitution& subst;

    SubstitutionContext(SemaContext& s, const GenericSubstitution& sub)
        : sema(s), subst(sub) {}
};

// ─── Substitution (implemented in Generic.cpp) ────────────────────────

TypeAST* substituteType(TypeAST* type, SubstitutionContext& sc);
StmtAST* substituteStmt(StmtAST* stmt, SubstitutionContext& sc);
ExprAST* substituteExpr(ExprAST* expr, SubstitutionContext& sc);

/// @brief Substitute generic parameters in a declaration.
///
/// A declaration can appear in two places that need substitution:
///
///   - Inside a `DeclStmt` in a generic function's body. The declaration's
///     type and initializer may reference `T`, and both need to be
///     substituted before the specialized body is resolved. This is the
///     path `substituteStmt`'s `DeclStmt` case takes.
///
///   - In principle, anywhere else a `DeclAST` can be reached through the
///     substitution walk. Today that's only the `DeclStmt` case, but the
///     helper is written to handle a full declaration regardless of how
///     it was reached.
///
/// Only the declaration kinds that can legally appear inside a function
/// body are handled here — `VarDeclAST`, `FuncDeclAST`, `StructDeclAST`,
/// `EnumDeclAST`, `TraitDeclAST`. Other kinds (imports, module-level
/// declarations reached through unusual paths) fall through unchanged.
DeclAST* substituteDecl(DeclAST* decl, SubstitutionContext& sc);

bool containsGenericParams(TypeAST* type, const GenericSubstitution& subst);

// ─── Instantiation (implemented in Instantiation.cpp) ─────────────────

/// @brief Result of resolving a generic instantiation.
struct GenericResolution {
    DeclAST* resolvedDecl = nullptr;
    // Add other fields here if the current GenericResolution has more.
};

// ─── Generic Resolution (declaration) ─────────────────────────────────────

/// @brief Resolve a generic instantiation.
/// 
/// This is the single decision point for generic instantiation: validate
/// arity, then create and return the specialized declaration for the concrete
/// type arguments.
/// 
/// @param templateDecl The generic declaration (FuncDeclAST* or StructDeclAST*).
/// @param typeArgs The concrete type arguments provided at the use site.
/// @param ctx The semantic context.
/// @return A GenericResolution containing the resolved declaration.
GenericResolution resolveGenericInstantiation(
    DeclAST* templateDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx);

/// @brief Canonicalize a list of type arguments.
///
/// Maps each parser-produced type node to the canonical node that
/// represents the same type in the type cache. Two call sites writing
/// `Box<int>` produce two distinct PrimitiveTypeAST(Int) nodes from the
/// parser; after canonicalization both produce the same singleton.
///
/// This is the single entry point both `resolveNamedType` (in SemaResolve.cpp)
/// and `resolveStructLiteralExpr` (in SemaExpr.cpp) must call before
/// looking up `SemaContext::genericTypeInstantiations`, since the storage
/// map is keyed on pointer identity of canonical args.
ArenaSpan<TypeAST*> canonicalizeTypeArgList(const ArenaSpan<TypeAST*>& args, SemaContext& ctx);

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