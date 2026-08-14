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

/// @brief Context for substituting generic parameters with concrete types.
struct GenericSubstitution {
    const ArenaSpan<GenericParamDeclAST*>& genericParams;
    const std::vector<TypeAST*>& typeArgs;

    TypeAST* lookup(InternedString name) const {
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name && i < typeArgs.size()) {
                return typeArgs[i];
            }
        }
        return nullptr;
    }

    bool isGenericParam(InternedString name) const {
        for (const auto* param : genericParams) {
            if (param->name == name) return true;
        }
        return false;
    }
};

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