/// @file CodeGenHelpers.hpp
/// @brief Helper functions for code generation - allocas, blocks, loads, panic, and generics.

#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <string>
#include <vector>

namespace codegen {

// ─── Forward Declaration ──────────────────────────────────────────────────
llvm::Type* getType(CodeGenContext& ctx, const TypeAST* type);

// ─── Alloca Creation ──────────────────────────────────────────────────────

llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
);

llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
);

// ─── Load Helpers ─────────────────────────────────────────────────────────

/// @brief Load a value from a pointer with explicit element type.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
);

/// @brief DEPRECATED: Use the overload with explicit elemType.
/// With opaque pointers (LLVM 17+), the element type cannot be inferred.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    bool isLValue,
    CodeGenContext& ctx
);

// ─── Panic ─────────────────────────────────────────────────────────────────

void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
);

llvm::Value* emitNullCheck(
    llvm::Value* ptr,
    const std::string& message,
    CodeGenContext& ctx
);

// ─── Type Helpers ─────────────────────────────────────────────────────────

llvm::Type* getDeclType(
    const ValueDeclAST* decl,
    CodeGenContext& ctx
);

// ─── Generic Helper Functions ────────────────────────────────────────────

/// @brief Check if a function has generic parameters.
bool isGenericFunction(const FuncDeclAST* decl);

/// @brief Check if a struct has generic parameters.
bool isGenericStruct(const StructDeclAST* decl);

/// @brief Check if a declaration should be specialized (user requested via @[specialize]).
bool shouldSpecialize(const DeclAST* decl);

/// @brief Check if a name matches any generic parameter.
bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams);

/// @brief Find the index of a generic parameter by name.
size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams);

/// @brief Substitute generic parameters in a type.
const TypeAST* substituteGenericType(
    const TypeAST* type,
    const ArenaSpan<GenericParamDeclPtr>& genericParams,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

} // namespace codegen