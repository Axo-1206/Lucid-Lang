/// @file CodeGenGeneric.hpp
/// @brief Generic instantiation handling for code generation.
///
/// ─── Generic Implementation Strategy ──────────────────────────────────────
/// Lucid uses a hybrid approach for generics:
///
///   1. DEFAULT: Type Erasure (Tagged Slots)
///      - One LLVM function per generic declaration
///      - All values passed as tagged slots { tag, value }
///      - Runtime tag checking for type safety
///      - Best for library code, many instantiations
///
///   2. OPT-IN: Monomorphization (@[specialize])
///      - Separate LLVM function per concrete type instantiation
///      - No runtime overhead, direct calls
///      - Best for hot loops, performance-critical code
///      - User controls with @[specialize] attribute
///
/// ─── Design ─────────────────────────────────────────────────────────────────
/// The generic registry caches all instantiations so we don't generate
/// the same specialized function twice. For type-erased generics, we
/// generate one function that works for all types using opaque pointers
/// and tagged slots.
///
/// ─── Type Erasure Implementation ──────────────────────────────────────────
/// All generic functions are lowered to a single LLVM function that:
///   1. Takes all parameters as opaque pointers (i8*)
///   2. Uses tagged slots { i8 tag, value } for type info
///   3. Runtime tag checks for safety
///   4. One function per generic declaration, regardless of instantiations
///
/// ─── Monomorphization Implementation ─────────────────────────────────────
/// When @[specialize] is present:
///   1. Each concrete instantiation generates a new LLVM function
///   2. Types are substituted directly (no tag indirection)
///   3. LLVM can inline and optimize per instantiation
///   4. Cached in genericRegistry to avoid duplication

#pragma once

#include "context/CodeGenContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/StringPool.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <unordered_map>
#include <string>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

/// @brief Check if a function has generic parameters.
bool isGenericFunction(const FuncDeclAST* decl);

/// @brief Check if a struct has generic parameters.
bool isGenericStruct(const StructDeclAST* decl);

/// @brief Check if a declaration should be specialized (user requested via @[specialize]).
bool shouldSpecialize(const DeclAST* decl);

/// @brief Check if a type is specializable (primitive or small struct).
bool isSpecializableType(const TypeAST* type, CodeGenContext& ctx);

/// @brief Get or create a specialized function instantiation.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The specialized LLVM function, or nullptr on error.
llvm::Function* getOrCreateSpecializedFunction(
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Get or create a specialized struct type.
/// @param structDecl The generic struct declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The specialized LLVM struct type, or nullptr on error.
llvm::Type* getOrCreateSpecializedStruct(
    const StructDeclAST* structDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Generate a mangled name for a generic instantiation.
/// @param baseName The base name of the declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The mangled name.
std::string mangleGenericName(
    const std::string& baseName,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Check if a generic parameter name matches a type argument.
bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams);

/// @brief Find the index of a generic parameter by name.
size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams);

/// @brief Substitute generic parameters in a type.
/// @param type The type containing generic parameters.
/// @param genericParams The generic parameters to substitute.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The substituted type (or nullptr if not a generic param).
const TypeAST* substituteGenericType(
    const TypeAST* type,
    const ArenaSpan<GenericParamDeclPtr>& genericParams,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Create a specialized LLVM function from a generic function.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The specialized LLVM function, or nullptr on error.
llvm::Function* createSpecializedFunction(
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Create a specialized LLVM struct type from a generic struct.
/// @param structDecl The generic struct declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The specialized LLVM struct type, or nullptr on error.
llvm::Type* createSpecializedStruct(
    const StructDeclAST* structDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Generate the type-erased (tagged slot) version of a generic function.
/// This is the default implementation for generic functions.
/// @param funcDecl The generic function declaration.
/// @param ctx The code generation context.
/// @return The LLVM function, or nullptr on error.
llvm::Function* generateErasedGenericFunction(
    const FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

/// @brief Generate the type-erased version of a generic struct.
/// This is the default implementation for generic structs.
/// @param structDecl The generic struct declaration.
/// @param ctx The code generation context.
/// @return The LLVM struct type, or nullptr on error.
llvm::Type* generateErasedGenericStruct(
    const StructDeclAST* structDecl,
    CodeGenContext& ctx
);

} // namespace codegen