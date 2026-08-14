/// @file CodeGenGeneric.hpp
/// @brief Generic instantiation handling for code generation.
///
/// This file provides:
///   1. Detection helpers: isGenericFunction, isGenericStruct, shouldSpecialize
///   2. Type substitution: GenericSubstitution, substituteGenericType
///   3. Instantiation: createSpecializedFunction, createSpecializedStruct
///   4. Type-erased generation: generateErasedGenericFunction, generateErasedGenericStruct
///   5. Registry access: getOrCreateSpecializedFunction, getOrCreateSpecializedStruct

#pragma once

#include "context/CodeGenContext.hpp"
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

/// @brief Check if a function has generic parameters.
bool isGenericFunction(FuncDeclAST* decl);

/// @brief Check if a struct has generic parameters.
bool isGenericStruct(StructDeclAST* decl);

/// @brief Check if a declaration should be specialized (user requested via @[specialize]).
bool shouldSpecialize(DeclAST* decl);

/// @brief Check if a name matches any generic parameter.
bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams);

/// @brief Find the index of a generic parameter by name.
size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclAST*>& genericParams);

// ─────────────────────────────────────────────────────────────────────────────
// 2. Type Substitution
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Context for substituting generic parameters with concrete types.
struct GenericSubstitution {
    const ArenaSpan<GenericParamDeclAST*>& genericParams;  // The generic parameter declarations
    const std::vector<TypeAST*>& typeArgs;          // The concrete type arguments

    /// @brief Find the type argument for a given generic parameter name.
    /// @param name The generic parameter name.
    /// @return The substituted type, or nullptr if not found.
    TypeAST* lookup(InternedString name) const {
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name && i < typeArgs.size()) {
                return typeArgs[i];
            }
        }
        return nullptr;
    }
};

/// @brief Substitute generic parameters in a type.
/// @deprecated Use GenericSubstitution with getType() instead.
/// @param type The type to substitute.
/// @param genericParams The generic parameter declarations.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The substituted type, or the original type if no substitution needed.
TypeAST* substituteGenericType(
    TypeAST* type,
    const ArenaSpan<GenericParamDeclAST*>& genericParams,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 3. Specialized Instantiation Creation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Create a specialized function for a generic function instantiation.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The LLVM function, or nullptr on error.
llvm::Function* createSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Create a specialized struct type for a generic struct instantiation.
/// @param structDecl The generic struct declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The LLVM struct type, or nullptr on error.
llvm::Type* createSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 4. Type-Erased Generic Generation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Generate the type-erased version of a generic function.
/// @param funcDecl The generic function declaration.
/// @param ctx The code generation context.
/// @return The LLVM function, or nullptr on error.
llvm::Function* generateErasedGenericFunction(
    FuncDeclAST* funcDecl,
    CodeGenContext& ctx
);

/// @brief Generate the type-erased version of a generic struct.
/// @param structDecl The generic struct declaration.
/// @param ctx The code generation context.
/// @return The LLVM struct type, or nullptr on error.
llvm::Type* generateErasedGenericStruct(
    StructDeclAST* structDecl,
    CodeGenContext& ctx
);

// ─────────────────────────────────────────────────────────────────────────────
// 5. Public Registry API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Get or create a specialized function instantiation.
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The LLVM function, or nullptr on error.
llvm::Function* getOrCreateSpecializedFunction(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

/// @brief Get or create a specialized struct type.
/// @param structDecl The generic struct declaration.
/// @param typeArgs The concrete type arguments.
/// @param ctx The code generation context.
/// @return The LLVM struct type, or nullptr on error.
llvm::Type* getOrCreateSpecializedStruct(
    StructDeclAST* structDecl,
    const std::vector<TypeAST*>& typeArgs,
    CodeGenContext& ctx
);

} // namespace codegen