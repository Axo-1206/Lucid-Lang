/// @file sema/types/GenericHelpers.hpp
/// @brief Helpers for checking generic type usage and validation.
///
/// This file provides utilities for determining if a type is a generic parameter,
/// contains generic parameters, or is being used in a type-erased context.
///
/// ─── Key Functions ──────────────────────────────────────────────────────────
/// - isGenericParameterType()      - Check if a type is T, U, etc.
/// - containsGenericParameter()    - Check if a type contains any generic params
/// - isTypeErasedGeneric()         - Check if a type is in a type-erased context
/// - isConcreteType()              - Check if a type is fully concrete
/// - validateConcreteTypeForReflection() - Validate for #sizeof/#alignof/#tostr
/// - validateConcreteTypeForSimd() - Validate for Simd<T,N>
/// - validateConcreteTypeForAlloc() - Validate for #alloc(T, count)
/// - validateConcreteTypeForBitcast() - Validate for #bitcast(T, x)
/// - validateTypeErasedEligibility() - Validate a generic declaration's body

#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "sema/context/SemaContext.hpp"
#include "core/diagnostics/DiagCode.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace sema {

// ─── Generic Parameter Detection ──────────────────────────────────────────

/// @brief Check if a type is a generic parameter (T, U, etc.).
///
/// A type is a generic parameter if:
/// - It's a NamedTypeAST whose resolvedDecl is a GenericParamDeclAST
/// - OR it's a NamedTypeAST whose name matches a generic param in the current scope
///
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type is a generic parameter.
bool isGenericParameterType(TypeAST* type, SemaContext& ctx);

/// @brief Check if a type contains any generic parameters at any depth.
///
/// This recursively walks through the type structure looking for any
/// generic parameters.
///
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type contains any generic parameters.
bool containsGenericParameter(TypeAST* type, SemaContext& ctx);

/// @brief Check if a type is being used in a type-erased context.
///
/// A type is in a type-erased context if:
/// - It's a generic parameter (T) being used in a function/struct that does NOT
///   have @[specialize]
/// - OR it's a type that contains a generic parameter, used in a non-specialized
///   context
///
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type is a type-erased generic.
bool isTypeErasedGeneric(TypeAST* type, SemaContext& ctx);

/// @brief Check if a type is fully concrete (not generic or fully specialized).
///
/// A type is concrete if:
/// - It is not a generic parameter
/// - It contains no generic parameters
/// - It is not being used in a type-erased context
///
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type is concrete.
bool isConcreteType(TypeAST* type, SemaContext& ctx);

/// @brief Get the innermost generic declaration containing the current context.
/// @param ctx The semantic context.
/// @return The generic declaration, or nullptr if not in one.
DeclAST* getInnermostGenericDeclaration(SemaContext& ctx);

/// @brief Check if the current context is specialized (@[specialize]).
/// @param ctx The semantic context.
/// @return true if the current context has @[specialize].
bool isCurrentContextSpecialized(SemaContext& ctx);

// ─── Reflection Validation ─────────────────────────────────────────────────

/// @brief Validate that a type is concrete for reflection intrinsics.
///
/// This is the main entry point for validating #sizeof, #alignof, #tostr, etc.
/// It checks that the type is fully concrete and not type-erased.
///
/// @param type The type to validate.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @param intrinsicName The name of the intrinsic for error messages.
/// @return true if the type is valid for reflection.
bool validateConcreteTypeForReflection(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx,
    const std::string& intrinsicName
);

/// @brief Validate that a type is concrete for SIMD operations.
///
/// @param type The type to validate.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @return true if the type is valid for SIMD.
bool validateConcreteTypeForSimd(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx
);

/// @brief Validate that a type is concrete for #alloc(T, count).
///
/// @param type The type to validate.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @return true if the type is valid for allocation.
bool validateConcreteTypeForAlloc(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx
);

/// @brief Validate that a type is concrete for #bitcast(T, x).
///
/// @param type The type to validate.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @return true if the type is valid for bitcasting.
bool validateConcreteTypeForBitcast(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx
);

/// @brief Validate that a type is concrete for arena::alloc<T>().
///
/// @param type The type to validate.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @return true if the type is valid for arena allocation.
bool validateConcreteTypeForArenaAlloc(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx
);

/// @brief Validate that a type is concrete for arena::space<T>() or arena::canFit<T>().
///
/// @param type The type to validate.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @param methodName The method name for error messages.
/// @return true if the type is valid for arena space/canFit.
bool validateConcreteTypeForArenaSpace(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx,
    const std::string& methodName
);

// ─── Generic Declaration Validation ───────────────────────────────────────

/// @brief Validate that a generic declaration is eligible for type-erasure.
///
/// This walks the declaration's body/fields to check for forbidden constructs:
///   - #sizeof/#alignof/#tostr on the generic parameter
///   - Simd<T,N> where T is the generic parameter
///   - #alloc(T, count) on the generic parameter
///   - arena::alloc<T> on the generic parameter
///   - Trait bounds on the generic parameter
///
/// If any forbidden construct is found, an error is reported suggesting @[specialize].
///
/// @param decl The generic declaration (FuncDeclAST or StructDeclAST).
/// @param ctx The semantic context.
/// @return true if the declaration is eligible for type-erasure.
bool validateTypeErasedEligibility(DeclAST* decl, SemaContext& ctx);

/// @brief Check if an expression contains forbidden constructs for type-erased generics.
///
/// @param expr The expression to check.
/// @param genericParamNames The names of generic parameters to check against.
/// @param ctx The semantic context.
/// @return true if the expression contains forbidden constructs.
bool containsForbiddenConstructs(
    ExprAST* expr,
    const std::unordered_set<InternedString>& genericParamNames,
    SemaContext& ctx
);

/// @brief Check if a statement contains forbidden constructs for type-erased generics.
///
/// @param stmt The statement to check.
/// @param genericParamNames The names of generic parameters to check against.
/// @param ctx The semantic context.
/// @return true if the statement contains forbidden constructs.
bool containsForbiddenConstructs(
    StmtAST* stmt,
    const std::unordered_set<InternedString>& genericParamNames,
    SemaContext& ctx
);

/// @brief Check if a type contains a generic parameter from the given set.
///
/// @param type The type to check.
/// @param genericParamNames The names of generic parameters to check against.
/// @param ctx The semantic context.
/// @return true if the type contains a generic parameter.
bool typeContainsGenericParam(
    TypeAST* type,
    const std::unordered_set<InternedString>& genericParamNames,
    SemaContext& ctx
);

// ─── Nested Generic Validation ────────────────────────────────────────────

/// @brief Validate that nested generic types are compatible with the erasure strategy.
///
/// This checks that if a type-erased generic contains a specialized generic,
/// it's a compile error (shape mismatch).
///
/// @param outerDecl The outer generic declaration.
/// @param typeArgs The type arguments being used.
/// @param ctx The semantic context.
/// @return true if all nested types are compatible.
bool validateNestedGenericCompatibility(
    DeclAST* outerDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    SemaContext& ctx
);

// ─── Diagnostic Helpers ────────────────────────────────────────────────────

/// @brief Emit an error suggesting @[specialize] for a type-erased generic.
///
/// @param type The type that caused the error.
/// @param node The AST node for error reporting.
/// @param ctx The semantic context.
/// @param featureName The name of the feature that requires specialization.
void emitTypeErasedError(
    TypeAST* type,
    BaseAST* node,
    SemaContext& ctx,
    const std::string& featureName
);

/// @brief Get the names of generic parameters from a declaration.
///
/// @param decl The generic declaration.
/// @param ctx The semantic context.
/// @return A set of generic parameter names.
std::unordered_set<InternedString> getGenericParamNames(
    DeclAST* decl,
    SemaContext& ctx
);

} // namespace sema