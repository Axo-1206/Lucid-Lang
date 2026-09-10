/// @file CodeGenGeneric.hpp
/// @brief Generic instantiation handling for code generation.
///
/// This file provides:
///   1. Detection helpers: isGenericFunction, isGenericStruct
///   2. Type-erased generation: generateErasedGenericFunction, generateErasedGenericStruct
///   3. Registry access: getOrCreateInstantiatedFunction, getOrCreateInstantiatedStruct
///
/// Sema now handles ALL instantiation decisions. CodeGen simply reads the
/// result. The flow is:
///
///   1. Sema calls resolveGenericInstantiation() which:
///      - If @[erased]: keeps the template with genericParams intact and
///        sets erasedName.
///      - Otherwise (default): creates a specialized decl with
///        genericParams = {} and sets mangledName.
///
///   2. CodeGen checks decl->genericParams:
///      - If empty: Sema already specialized it → lookup by mangled name.
///      - If non-empty: type-erased path (@[erased]) → use erasedName.
///
///   CodeGen NO LONGER checks isErased() - that decision is final
///   and already made by Sema.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace codegen {

// ─── Detection Helpers ────────────────────────────────────────────────────

/// @brief Check if a function declaration is generic (has generic parameters).
bool isGenericFunction(FuncDeclAST* decl);

/// @brief Check if a struct declaration is generic (has generic parameters).
bool isGenericStruct(StructDeclAST* decl);

/// @brief Check if a declaration has the @[erased] attribute.
/// @note This is a READ-ONLY informational check. CodeGen should NOT use
///       this to make decisions - use decl->genericParams.empty() instead.
bool isErased(DeclAST* decl);

/// @brief Check if a type is a generic parameter (T, U, etc.)
bool isGenericParameterType(TypeAST* type);

// ─── Type-Erased Generation ──────────────────────────────────────────────

/// @brief Generate a type-erased generic function.
/// 
/// This is used only for the @[erased] path.
/// The function takes all parameters as TaggedSlot* and returns TaggedSlot*.
/// 
/// @param funcDecl The generic function declaration.
/// @param ctx The code generation context.
/// @return The LLVM function, or nullptr on error.
llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

/// @brief Generate a type-erased generic struct.
/// 
/// This is used only for the @[erased] path.
/// All fields are TaggedSlot (tag byte + opaque pointer).
/// 
/// @param structDecl The generic struct declaration.
/// @param ctx The code generation context.
/// @return The LLVM struct type, or nullptr on error.
llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
);

// ─── Registry Access ──────────────────────────────────────────────────────

/// @brief Get or create a specialized function.
///
/// This is the main entry point for resolving generic function calls.
/// It handles both paths:
///   - Specialized (default): Sema already created the decl with
///     genericParams = {} and mangledName set. CodeGen just looks it up.
///   - Type-erased (@[erased]): Generates the erased version.
///
/// @param funcDecl The function declaration (template or specialized).
/// @param typeArgs The type arguments (for cache key, if needed).
/// @param ctx The code generation context.
/// @return The LLVM function, or nullptr on error.
llvm::Function* getOrCreateInstantiatedFunction(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Get or create a specialized struct type.
///
/// This is the main entry point for resolving generic struct types.
/// It handles both paths:
///   - Specialized (default): Sema already created the decl with
///     genericParams = {} and mangledName set. CodeGen just looks it up.
///   - Type-erased (@[erased]): Generates the erased version.
///
/// @param structDecl The struct declaration (template or specialized).
/// @param typeArgs The type arguments (for cache key, if needed).
/// @param ctx The code generation context.
/// @return The LLVM struct type, or nullptr on error.
llvm::Type* getOrCreateInstantiatedStruct(
    StructDeclAST* structDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Resolve a generic call to a function.
///
/// Convenience wrapper for getOrCreateInstantiatedFunction with error handling.
///
/// @param funcDecl The function declaration.
/// @param genericArgs The generic arguments.
/// @param ctx The code generation context.
/// @param loc The source location for error reporting.
/// @return The LLVM function, or nullptr on error.
llvm::Value* resolveGenericCall(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& genericArgs,
    CodeGenContext& ctx,
    const SourceLocation& loc
);

} // namespace codegen