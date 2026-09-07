/// @file CodeGenGeneric.hpp
/// @brief Generic instantiation handling for code generation.
///
/// This file provides:
///   1. Detection helpers: isGenericFunction, isGenericStruct, shouldSpecialize
///   2. Type substitution: GenericSubstitution
///   3. Instantiation: createSpecializedFunction, createSpecializedStruct
///   4. Type-erased generation: generateErasedGenericFunction, generateErasedGenericStruct
///   5. Registry access: getOrCreateSpecializedFunction, getOrCreateSpecializedStruct
///
/// ───────────────────────────────────────────────────────────────────────────
/// MEMORY MODEL
/// ───────────────────────────────────────────────────────────────────────────
///
/// All generic APIs use ArenaSpan<TypeAST*> for type arguments, matching the
/// AST's memory model. This means:
///   - No heap allocation at call sites
///   - No conversion from ArenaSpan to vector
///   - Direct pass-through of AST data
///   - The arena outlives all generic operations

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace codegen {

// ─── Detection Helpers ────────────────────────────────────────────────────

bool isGenericFunction(FuncDeclAST* decl);
bool isGenericStruct(StructDeclAST* decl);
bool shouldSpecialize(DeclAST* decl);
bool isGenericParameterName(
    InternedString name,
    const ArenaSpan<GenericParamDeclAST*>& genericParams
);

// ─── Generic Parameter Helpers (need context for substitution) ─────────

/// @brief Check if a type is a generic parameter in the given substitution context.
inline bool isGenericParameterType(TypeAST* type, const GenericSubstitution* subst) {
    if (!type || !subst) return false;
    if (!type->isa<NamedTypeAST>()) return false;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    return subst->isGenericParam(named->name);
}

/// @brief Get the substituted type for a generic parameter, if any.
inline TypeAST* getSubstitutedType(TypeAST* type, const GenericSubstitution* subst) {
    if (!type || !subst) return nullptr;
    if (!type->isa<NamedTypeAST>()) return nullptr;
    NamedTypeAST* named = type->as<NamedTypeAST>();
    return subst->lookup(named->name);
}

/// @brief Check if a type contains any generic parameters at any depth.
bool containsGenericParameter(TypeAST* type, const GenericSubstitution* subst);

// ─── Instantiation API ────────────────────────────────────────────────────

llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
);

/// This is the main entry point for resolving generic function calls.
/// It handles both:
///   - @[specialize]: Creates a monomorphized version for the specific types.
///   - Default: Returns the type-erased function.
llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// This is the main entry point for resolving generic struct types.
/// It handles both:
///   - @[specialize]: Creates a monomorphized version for the specific types.
///   - Default: Returns the type-erased struct type.
llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

llvm::Value* resolveGenericCall(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& genericArgs,
    CodeGenContext& ctx,
    const SourceLocation& loc
);

/// This is called from getOrCreateSpecializedFunction to record each
/// instantiation for use by #sizeof/#alignof/#tostr intrinsics.
void recordGenericInstantiation(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

std::vector<ArenaSpan<TypeAST*>> getRecordedInstantiations(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

} // namespace codegen