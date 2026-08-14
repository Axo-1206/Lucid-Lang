/// @file CodeGenType.hpp
/// @brief Type mapping from Lucid AST types to LLVM types.
/// 
/// This file declares the type mapping functions used by CodeGenContext.
/// It maps Lucid's type system (primitives, structs, enums, arrays, pointers,
/// references, function types, nullable/fallible types) to LLVM IR types.
///
/// ─── Design Principles ──────────────────────────────────────────────────────
///   1. Types are cached in CodeGenContext for performance
///   2. Named types return the struct type (not a pointer) - pointers are
///      created separately when needed (e.g., for function parameters)
///   3. Nullable/fallible/combined are always structs { i8 tag, T value }
///   4. Arrays: fixed = LLVM array, dynamic = pointer, slice = { ptr, len, cap }

#pragma once

#include "context/CodeGenContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include <llvm/IR/DerivedTypes.h>
#include <unordered_map>

namespace codegen {

// ─── Forward Declaration ──────────────────────────────────────────────────

struct GenericSubstitution;

// ─── Main Type Mapping ─────────────────────────────────────────────────────

/// @brief Get the LLVM type for a Lucid type annotation.
/// @param ctx The code generation context.
/// @param type The Lucid type annotation.
/// @return The LLVM type, or nullptr if the type cannot be mapped.
llvm::Type* getType(CodeGenContext& ctx, TypeAST* type);

/// @brief Get the LLVM type for a Lucid type annotation with generic substitution.
/// @param ctx The code generation context.
/// @param type The Lucid type annotation.
/// @param subst The generic substitution context (optional).
/// @return The LLVM type, or nullptr if the type cannot be mapped.
llvm::Type* getType(
    CodeGenContext& ctx,
    TypeAST* type,
    const GenericSubstitution* subst
);

/// @brief Get the LLVM struct type for a Lucid struct declaration.
/// @param ctx The code generation context.
/// @param decl The struct declaration.
/// @return The LLVM struct type, or nullptr if the struct cannot be mapped.
llvm::StructType* getStructType(CodeGenContext& ctx, StructDeclAST* decl);

/// @brief Get the LLVM type for a Lucid enum declaration (backing integer type).
/// @param ctx The code generation context.
/// @param decl The enum declaration.
/// @return The LLVM integer type, or nullptr if the enum cannot be mapped.
llvm::IntegerType* getEnumType(CodeGenContext& ctx, const EnumDeclAST* decl);

/// @brief Get the LLVM function type for a Lucid function type.
/// @param ctx The code generation context.
/// @param funcType The Lucid function type.
/// @param isClosure Whether this is a closure function (adds env pointer).
/// @return The LLVM function type, or nullptr if the function type cannot be mapped.
llvm::FunctionType* getFunctionType(
    CodeGenContext& ctx,
    FuncTypeAST* funcType,
    bool isClosure = false
);

// ─── Specific Type Mappers ──────────────────────────────────────────────────

/// @brief Get the LLVM type for a Lucid primitive type.
llvm::Type* getPrimitiveType(CodeGenContext& ctx, PrimitiveTypeAST* type);

/// @brief Get the LLVM type for a Lucid named type (struct or primitive alias).
llvm::Type* getNamedType(CodeGenContext& ctx, NamedTypeAST* type);

/// @brief Get the LLVM type for a Lucid pointer type (*T).
llvm::Type* getPtrType(CodeGenContext& ctx, PtrTypeAST* type);

/// @brief Get the LLVM type for a Lucid reference type (&T).
llvm::Type* getRefType(CodeGenContext& ctx, RefTypeAST* type);

/// @brief Get the LLVM type for a Lucid array type.
/// @param ctx The code generation context.
/// @param type The array type.
/// @param subst Optional generic substitution context.
/// @return The LLVM array type.
llvm::Type* getArrayType(
    CodeGenContext& ctx,
    ArrayTypeAST* type,
    const GenericSubstitution* subst = nullptr
);

/// @brief Get the LLVM type for a Lucid nullable type (T?).
/// @param ctx The code generation context.
/// @param type The nullable type.
/// @param subst Optional generic substitution context.
/// @return A struct type { i8 tag, T value }.
llvm::StructType* getNullableType(
    CodeGenContext& ctx,
    NullableTypeAST* type,
    const GenericSubstitution* subst = nullptr
);

/// @brief Get the LLVM type for a Lucid fallible type (T!).
/// @param ctx The code generation context.
/// @param type The fallible type.
/// @param subst Optional generic substitution context.
/// @return A struct type { i8 tag, T value }.
llvm::StructType* getFallibleType(
    CodeGenContext& ctx,
    FallibleTypeAST* type,
    const GenericSubstitution* subst = nullptr
);

/// @brief Get the LLVM type for a Lucid combined type (T?!).
/// @param ctx The code generation context.
/// @param type The combined type.
/// @param subst Optional generic substitution context.
/// @return A struct type { i8 tag, T value } (tag encodes nil/err/value).
llvm::StructType* getCombinedType(
    CodeGenContext& ctx,
    CombinedTypeAST* type,
    const GenericSubstitution* subst = nullptr
);

/// @brief Get the LLVM type for a Lucid future type (Future<T>).
/// @param ctx The code generation context.
/// @param type The future type.
/// @param subst Optional generic substitution context.
/// @return A struct type { T value, i8 state }.
llvm::StructType* getFutureType(
    CodeGenContext& ctx,
    FutureTypeAST* type,
    const GenericSubstitution* subst = nullptr
);

/// @brief Get the LLVM type for a Lucid thread type (Thread<T>).
/// @param ctx The code generation context.
/// @param type The thread type.
/// @param subst Optional generic substitution context.
/// @return A struct type { T value, i8 state }.
llvm::StructType* getThreadType(
    CodeGenContext& ctx,
    const ThreadTypeAST* type,
    const GenericSubstitution* subst = nullptr
);

/// @brief Get the LLVM type for a Lucid module type access.
llvm::Type* getModuleTypeAccess(CodeGenContext& ctx, ModuleTypeAccessAST* type);

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// @brief Get the integer type for a primitive kind.
llvm::IntegerType* getIntegerType(CodeGenContext& ctx, PrimitiveKind kind);

/// @brief Get the floating-point type for a primitive kind.
llvm::Type* getFloatType(CodeGenContext& ctx, PrimitiveKind kind);

/// @brief Get the name of a Lucid type as a string.
std::string getTypeName(CodeGenContext& ctx, TypeAST* type);

/// @brief Get the size of a Lucid type in bytes (compile-time).
uint64_t getTypeSize(CodeGenContext& ctx, TypeAST* type);

/// @brief Get the alignment of a Lucid type in bytes (compile-time).
uint64_t getTypeAlign(CodeGenContext& ctx, TypeAST* type);

} // namespace codegen