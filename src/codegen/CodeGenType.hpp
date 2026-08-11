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

// ─── Main Type Mapping ─────────────────────────────────────────────────────

/// @brief Get the LLVM type for a Lucid type annotation.
/// @param ctx The code generation context.
/// @param type The Lucid type annotation.
/// @return The LLVM type, or nullptr if the type cannot be mapped.
llvm::Type* getType(CodeGenContext& ctx, const TypeAST* type);

/// @brief Get the LLVM struct type for a Lucid struct declaration.
/// @param ctx The code generation context.
/// @param decl The struct declaration.
/// @return The LLVM struct type, or nullptr if the struct cannot be mapped.
llvm::StructType* getStructType(CodeGenContext& ctx, const StructDeclAST* decl);

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
    const FuncTypeAST* funcType,
    bool isClosure = false
);

// ─── Specific Type Mappers ──────────────────────────────────────────────────

/// @brief Get the LLVM type for a Lucid primitive type.
llvm::Type* getPrimitiveType(CodeGenContext& ctx, const PrimitiveTypeAST* type);

/// @brief Get the LLVM type for a Lucid named type (struct or primitive alias).
llvm::Type* getNamedType(CodeGenContext& ctx, const NamedTypeAST* type);

/// @brief Get the LLVM type for a Lucid pointer type (*T).
llvm::Type* getPtrType(CodeGenContext& ctx, const PtrTypeAST* type);

/// @brief Get the LLVM type for a Lucid reference type (&T).
llvm::Type* getRefType(CodeGenContext& ctx, const RefTypeAST* type);

/// @brief Get the LLVM type for a Lucid array type.
llvm::Type* getArrayType(CodeGenContext& ctx, const ArrayTypeAST* type);

/// @brief Get the LLVM type for a Lucid nullable type (T?).
/// @return A struct type { i8 tag, T value }.
llvm::StructType* getNullableType(CodeGenContext& ctx, const NullableTypeAST* type);

/// @brief Get the LLVM type for a Lucid fallible type (T!).
/// @return A struct type { i8 tag, T value }.
llvm::StructType* getFallibleType(CodeGenContext& ctx, const FallibleTypeAST* type);

/// @brief Get the LLVM type for a Lucid combined type (T?!).
/// @return A struct type { i8 tag, T value } (tag encodes nil/err/value).
llvm::StructType* getCombinedType(CodeGenContext& ctx, const CombinedTypeAST* type);

/// @brief Get the LLVM type for a Lucid future type (Future<T>).
/// @return A struct type { T value, i8 state }.
llvm::StructType* getFutureType(CodeGenContext& ctx, const FutureTypeAST* type);

/// @brief Get the LLVM type for a Lucid thread type (Thread<T>).
/// @return A struct type { T value, i8 state }.
llvm::StructType* getThreadType(CodeGenContext& ctx, const ThreadTypeAST* type);

/// @brief Get the LLVM type for a Lucid module type access.
llvm::Type* getModuleTypeAccess(CodeGenContext& ctx, const ModuleTypeAccessAST* type);

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// @brief Get the integer type for a primitive kind.
llvm::IntegerType* getIntegerType(CodeGenContext& ctx, PrimitiveKind kind);

/// @brief Get the floating-point type for a primitive kind.
llvm::Type* getFloatType(CodeGenContext& ctx, PrimitiveKind kind);

/// @brief Get the name of a Lucid type as a string.
std::string getTypeName(CodeGenContext& ctx, const TypeAST* type);

/// @brief Get the size of a Lucid type in bytes (compile-time).
uint64_t getTypeSize(CodeGenContext& ctx, const TypeAST* type);

/// @brief Get the alignment of a Lucid type in bytes (compile-time).
uint64_t getTypeAlign(CodeGenContext& ctx, const TypeAST* type);

} // namespace codegen