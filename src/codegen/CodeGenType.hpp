/// @file CodeGenType.hpp
/// @brief Type mapping from Lucid AST types to LLVM types.
/// 
/// This file declares the type mapping functions used by CodeGenContext.
/// It maps Lucid's type system (primitives, structs, enums, arrays, pointers,
/// references, function types, nullable/fallible types) to LLVM IR types.

#pragma once

#include "context/CodeGenContext.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include <llvm/IR/DerivedTypes.h>
#include <unordered_map>

namespace codegen {

// ─── Forward Declarations ──────────────────────────────────────────────────

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

/// @brief Get the LLVM function type for a Lucid function type.
/// @param ctx The code generation context.
/// @param funcType The Lucid function type.
/// @return The LLVM function type, or nullptr if the function type cannot be mapped.
llvm::FunctionType* getFunctionType(CodeGenContext& ctx, const FuncTypeAST* funcType);

/// @brief Get the LLVM type for a Lucid primitive type.
/// @param ctx The code generation context.
/// @param type The primitive type.
/// @return The LLVM type, or nullptr if the primitive type is unknown.
llvm::Type* getPrimitiveType(CodeGenContext& ctx, const PrimitiveTypeAST* type);

/// @brief Get the LLVM type for a Lucid named type.
/// @param ctx The code generation context.
/// @param type The named type.
/// @return The LLVM type (pointer to the struct), or nullptr if the type is unknown.
llvm::Type* getNamedType(CodeGenContext& ctx, const NamedTypeAST* type);

/// @brief Get the LLVM type for a Lucid pointer type (*T).
/// @param ctx The code generation context.
/// @param type The pointer type.
/// @return The LLVM pointer type.
llvm::Type* getPtrType(CodeGenContext& ctx, const PtrTypeAST* type);

/// @brief Get the LLVM type for a Lucid reference type (&T).
/// @param ctx The code generation context.
/// @param type The reference type.
/// @return The LLVM pointer type (references are pointers).
llvm::Type* getRefType(CodeGenContext& ctx, const RefTypeAST* type);

/// @brief Get the LLVM type for a Lucid array type.
/// @param ctx The code generation context.
/// @param type The array type.
/// @return The LLVM array or pointer type.
llvm::Type* getArrayType(CodeGenContext& ctx, const ArrayTypeAST* type);

/// @brief Get the LLVM type for a Lucid nullable type (T?).
/// @param ctx The code generation context.
/// @param type The nullable type.
/// @return A struct type { i8 tag, T value }.
llvm::StructType* getNullableType(CodeGenContext& ctx, const NullableTypeAST* type);

/// @brief Get the LLVM type for a Lucid fallible type (T!).
/// @param ctx The code generation context.
/// @param type The fallible type.
/// @return A struct type { i8 tag, T value }.
llvm::StructType* getFallibleType(CodeGenContext& ctx, const FallibleTypeAST* type);

/// @brief Get the LLVM type for a Lucid combined type (T?!).
/// @param ctx The code generation context.
/// @param type The combined type.
/// @return A struct type { i8 tag, T value } (tag encodes nil/err/value).
llvm::StructType* getCombinedType(CodeGenContext& ctx, const CombinedTypeAST* type);

/// @brief Get the LLVM type for a Lucid module type access.
/// @param ctx The code generation context.
/// @param type The module type access.
/// @return The resolved LLVM type.
llvm::Type* getModuleTypeAccess(CodeGenContext& ctx, const ModuleTypeAccessAST* type);

/// @brief Get the integer type for a primitive kind.
/// @param ctx The code generation context.
/// @param kind The primitive kind.
/// @return The LLVM integer type.
llvm::IntegerType* getIntegerType(CodeGenContext& ctx, PrimitiveKind kind);

/// @brief Get the floating-point type for a primitive kind.
/// @param ctx The code generation context.
/// @param kind The primitive kind.
/// @return The LLVM floating-point type.
llvm::Type* getFloatType(CodeGenContext& ctx, PrimitiveKind kind);

/// @brief Get the name of a Lucid type as a string.
/// @param ctx The code generation context.
/// @param type The Lucid type.
/// @return The type name.
std::string getTypeName(CodeGenContext& ctx, const TypeAST* type);

/// @brief Get the size of a Lucid type in bytes (compile-time).
/// @param ctx The code generation context.
/// @param type The Lucid type.
/// @return The size in bytes, or 0 if the type has unknown size.
uint64_t getTypeSize(CodeGenContext& ctx, const TypeAST* type);

/// @brief Get the alignment of a Lucid type in bytes (compile-time).
/// @param ctx The code generation context.
/// @param type The Lucid type.
/// @return The alignment in bytes, or 0 if the type has unknown alignment.
uint64_t getTypeAlign(CodeGenContext& ctx, const TypeAST* type);

} // namespace codegen