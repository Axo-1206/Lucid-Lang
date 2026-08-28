/// @file generic/GenericRegistry.hpp
/// @brief Registry for generic function and struct instantiations.
///
/// This registry tracks which specialized versions of generic functions and
/// structs have already been generated, preventing duplicate instantiations.
/// It is a CACHE, not a global registry - it lives per CodeGenContext.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <unordered_map>
#include <vector>

namespace codegen {

// ─── Forward declarations ──────────────────────────────────────────────────

/// @brief A key for identifying a generic instantiation.
///
/// Uniquely identifies a specific instantiation of a generic declaration
/// by its declaration and the concrete type arguments.
struct GenericInstantiationKey {
    DeclAST* decl;                    ///< The generic declaration
    std::vector<TypeAST*> typeArgs;   ///< Concrete type arguments

    bool operator==(const GenericInstantiationKey& other) const {
        if (decl != other.decl) return false;
        if (typeArgs.size() != other.typeArgs.size()) return false;
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (typeArgs[i] != other.typeArgs[i]) return false;
        }
        return true;
    }
};

/// @brief Hash for GenericInstantiationKey.
struct GenericInstantiationKeyHash {
    size_t operator()(const GenericInstantiationKey& key) const {
        size_t h1 = std::hash<DeclAST*>{}(key.decl);
        size_t h2 = 0;
        for (TypeAST* t : key.typeArgs) {
            h2 ^= std::hash<TypeAST*>{}(t) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
        }
        return h1 ^ (h2 << 1);
    }
};

/// @brief Registry of all generic instantiations in a module.
///
/// This is a CACHE, not a global registry. It tracks which specialized
/// versions we've already generated so we don't generate them twice.
///
/// ─── Function Instantiations ──────────────────────────────────────────────
/// Generic function → (type args → specialized function)
///
/// ─── Struct Instantiations ────────────────────────────────────────────────
/// Generic struct → (type args → specialized struct type)
struct GenericRegistry {
    // ─── Function Instantiations ──────────────────────────────────────────
    std::unordered_map<
        FuncDeclAST*,
        std::unordered_map<GenericInstantiationKey, llvm::Function*, GenericInstantiationKeyHash>
    > functionInstantiations;

    // ─── Struct Instantiations ─────────────────────────────────────────────
    std::unordered_map<
        StructDeclAST*,
        std::unordered_map<GenericInstantiationKey, llvm::Type*, GenericInstantiationKeyHash>
    > structInstantiations;

    /// @brief Check if a function instantiation already exists.
    bool hasFunctionInstantiation(FuncDeclAST* decl, const std::vector<TypeAST*>& typeArgs) const {
        auto it = functionInstantiations.find(decl);
        if (it == functionInstantiations.end()) return false;

        GenericInstantiationKey key{decl, typeArgs};
        return it->second.find(key) != it->second.end();
    }

    /// @brief Get a function instantiation if it exists.
    llvm::Function* getFunctionInstantiation(FuncDeclAST* decl, const std::vector<TypeAST*>& typeArgs) const {
        auto it = functionInstantiations.find(decl);
        if (it == functionInstantiations.end()) return nullptr;

        GenericInstantiationKey key{decl, typeArgs};
        auto found = it->second.find(key);
        return found != it->second.end() ? found->second : nullptr;
    }

    /// @brief Store a function instantiation.
    void storeFunctionInstantiation(FuncDeclAST* decl, const std::vector<TypeAST*>& typeArgs, llvm::Function* func) {
        GenericInstantiationKey key{decl, typeArgs};
        functionInstantiations[decl][key] = func;
    }

    /// @brief Check if a struct instantiation already exists.
    bool hasStructInstantiation(StructDeclAST* decl, const std::vector<TypeAST*>& typeArgs) const {
        auto it = structInstantiations.find(decl);
        if (it == structInstantiations.end()) return false;

        GenericInstantiationKey key{decl, typeArgs};
        return it->second.find(key) != it->second.end();
    }

    /// @brief Get a struct instantiation if it exists.
    llvm::Type* getStructInstantiation(StructDeclAST* decl, const std::vector<TypeAST*>& typeArgs) const {
        auto it = structInstantiations.find(decl);
        if (it == structInstantiations.end()) return nullptr;

        GenericInstantiationKey key{decl, typeArgs};
        auto found = it->second.find(key);
        return found != it->second.end() ? found->second : nullptr;
    }

    /// @brief Store a struct instantiation.
    void storeStructInstantiation(StructDeclAST* decl, const std::vector<TypeAST*>& typeArgs, llvm::Type* type) {
        GenericInstantiationKey key{decl, typeArgs};
        structInstantiations[decl][key] = type;
    }

    /// @brief Clear all instantiations (for hot-reload).
    void clear() {
        functionInstantiations.clear();
        structInstantiations.clear();
    }
};

} // namespace codegen