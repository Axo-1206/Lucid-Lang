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
#include "core/trace/Trace.hpp"
#include "core/memory/ArenaSpan.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <unordered_map>
#include <string>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// 1. Detection Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a function declaration is generic.
/// @param decl The function declaration.
/// @return True if the function has generic parameters.
bool isGenericFunction(FuncDeclAST* decl);

/// @brief Check if a struct declaration is generic.
/// @param decl The struct declaration.
/// @return True if the struct has generic parameters.
bool isGenericStruct(StructDeclAST* decl);

/// @brief Check if a declaration should be specialized (monomorphized).
/// @param decl The declaration.
/// @return True if @[specialize] is present.
bool shouldSpecialize(DeclAST* decl);

/// @brief Check if a name matches any generic parameter in a list.
/// @param name The name to check.
/// @param genericParams The list of generic parameters.
/// @return True if the name is a generic parameter.
bool isGenericParameterName(
    InternedString name,
    const ArenaSpan<GenericParamDeclAST*>& genericParams
);

// ─────────────────────────────────────────────────────────────────────────────
// 2. Specialized Instantiation Creation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create a specialized function for a specific instantiation.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments (from ArenaSpan).
/// @param ctx The code generation context.
/// @return The specialized LLVM function, or nullptr on error.
llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Create a specialized struct for a specific instantiation.
/// @param structDecl The generic struct declaration.
/// @param typeArgs The concrete type arguments (from ArenaSpan).
/// @param ctx The code generation context.
/// @return The specialized LLVM struct type, or nullptr on error.
llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 3. Type-Erased Generic Generation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Generate a type-erased version of a generic function.
/// @param funcDecl The generic function declaration.
/// @param ctx The code generation context.
/// @return The type-erased LLVM function, or nullptr on error.
llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

/// @brief Generate a type-erased version of a generic struct.
/// @param structDecl The generic struct declaration.
/// @param ctx The code generation context.
/// @return The type-erased LLVM struct type, or nullptr on error.
llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 4. Public Registry API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Get or create a specialized function for a generic function call.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments (from ArenaSpan).
/// @param ctx The code generation context.
/// @return The specialized or type-erased function, or nullptr on error.
///
/// This is the main entry point for resolving generic function calls.
/// It handles both:
///   - @[specialize]: Creates a monomorphized version for the specific types.
///   - Default: Returns the type-erased function.
llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Get or create a specialized struct for a generic struct use.
/// @param structDecl The generic struct declaration.
/// @param typeArgs The concrete type arguments (from ArenaSpan).
/// @param ctx The code generation context.
/// @return The specialized or type-erased struct type, or nullptr on error.
///
/// This is the main entry point for resolving generic struct types.
/// It handles both:
///   - @[specialize]: Creates a monomorphized version for the specific types.
///   - Default: Returns the type-erased struct type.
llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Resolve a generic function call.
/// @param funcDecl The function declaration.
/// @param genericArgs The generic arguments (from ArenaSpan).
/// @param ctx The code generation context.
/// @param loc The source location for error reporting.
/// @return The resolved LLVM function, or nullptr on error.
///
/// This is a convenience wrapper that handles:
///   - Non-generic functions: Returns the regular function.
///   - Generic functions: Validates arity and calls getOrCreateSpecializedFunction.
///   - Arity mismatch: Reports a clear error.
llvm::Value* resolveGenericCall(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& genericArgs,
    CodeGenContext& ctx,
    const SourceLocation& loc
);

// ─────────────────────────────────────────────────────────────────────────────
// 5. Reflection Support
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Record a generic instantiation for reflection.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments (from ArenaSpan).
/// @param ctx The code generation context.
///
/// This is called from getOrCreateSpecializedFunction to record each
/// instantiation for use by #sizeof/#alignof/#tostr intrinsics.
void recordGenericInstantiation(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Get all recorded instantiations for a generic function.
/// @param funcDecl The generic function declaration.
/// @param ctx The code generation context.
/// @return A vector of type argument spans.
std::vector<ArenaSpan<TypeAST*>> getRecordedInstantiations(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

} // namespace codegen