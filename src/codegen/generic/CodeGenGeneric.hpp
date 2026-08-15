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
#include "GenericSubstitution.hpp"
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
size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams);

// ─────────────────────────────────────────────────────────────────────────────
// 2. Type Substitution Context
// ─────────────────────────────────────────────────────────────────────────────
//
// GenericSubstitution now lives in GenericSubstitution.hpp (included above) -
// it moved out of this file because GenericMangledName.hpp/cpp also need the
// full definition, and having either of these two files include the other
// would form a cycle. See GenericSubstitution.hpp for the full rationale.

// ─────────────────────────────────────────────────────────────────────────────
// 3. Specialized Instantiation Creation
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
// 4. Type-Erased Generic Generation
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
// 5. Public Registry API
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