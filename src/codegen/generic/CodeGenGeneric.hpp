/// @file CodeGenGeneric.hpp
/// @brief Generic instantiation handling for code generation.
///
/// This file provides:
///   1. Detection helpers: isGenericFunction, isGenericStruct, shouldSpecialize
///   2. Type substitution: GenericSubstitution
///   3. Instantiation: createSpecializedFunction, createSpecializedStruct
///   4. Type-erased generation: generateErasedGenericFunction, generateErasedGenericStruct
///   5. Registry access: getOrCreateSpecializedFunction, getOrCreateSpecializedStruct

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/trace/Trace.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>
#include <unordered_map>
#include <string>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// 1. Detection Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool isGenericFunction(FuncDeclAST* decl);
bool isGenericStruct(StructDeclAST* decl);
bool shouldSpecialize(DeclAST* decl);
bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams);

// ─────────────────────────────────────────────────────────────────────────────
// 2. Specialized Instantiation Creation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 3. Type-Erased Generic Generation
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 4. Public Registry API
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

} // namespace codegen