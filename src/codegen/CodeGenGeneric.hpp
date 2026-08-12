/// @file CodeGenGeneric.hpp
/// @brief Generic instantiation handling for code generation.

#pragma once

#include "context/CodeGenContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "support/CodeGenHelpers.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <unordered_map>
#include <string>

namespace codegen {

// ─── Public API ────────────────────────────────────────────────────────────

/// @brief Get or create a specialized function instantiation.
llvm::Function* getOrCreateSpecializedFunction(
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Get or create a specialized struct type.
llvm::Type* getOrCreateSpecializedStruct(
    const StructDeclAST* structDecl,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Generate the type-erased version of a generic function.
llvm::Function* generateErasedGenericFunction(
    const FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

/// @brief Generate the type-erased version of a generic struct.
llvm::Type* generateErasedGenericStruct(
    const StructDeclAST* structDecl,
    CodeGenContext& ctx
);

} // namespace codegen