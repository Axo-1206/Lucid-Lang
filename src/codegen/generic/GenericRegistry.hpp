/// @file generic/GenericRegistry.hpp
/// @brief Registry for generic function and struct instantiations.
///
/// This registry tracks which specialized versions of generic functions and
/// structs have already been generated, preventing duplicate instantiations.
/// It is a CACHE, not a global registry - it lives per CodeGenContext.
///
/// ───────────────────────────────────────────────────────────────────────────
/// MEMORY MODEL
/// ───────────────────────────────────────────────────────────────────────────
///
/// GenericInstantiationKey stores `ArenaSpan<TypeAST*>` for type arguments,
/// matching the AST's memory model. This means:
///   - No heap allocation for keys
///   - Keys are trivially copyable (just pointer + size)
///   - Type ASTs are arena-allocated and outlive the registry
///   - The registry stores LLVM objects which ARE heap-allocated (by LLVM)
///
/// This is a deliberate hybrid: AST data lives in the arena, LLVM objects
/// are managed by LLVM's own allocator.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <unordered_map>
#include <vector>

namespace codegen {

// ─── Forward declarations ──────────────────────────────────────────────────

/// @brief A key for identifying a generic instantiation.
///
/// Uniquely identifies a specific instantiation of a generic declaration
/// by its declaration and the concrete type arguments.
///
/// Uses ArenaSpan for typeArgs, matching the AST's memory model.
/// No heap allocation is needed for keys.
struct GenericInstantiationKey {
    DeclAST* decl;                      ///< The generic declaration
    ArenaSpan<TypeAST*> typeArgs;       ///< Concrete type arguments (arena-allocated)

    /// @brief Equality comparison.
    bool operator==(const GenericInstantiationKey& other) const {
        if (decl != other.decl) return false;
        if (typeArgs.size() != other.typeArgs.size()) return false;
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (typeArgs[i] != other.typeArgs[i]) return false;
        }
        return true;
    }

    /// @brief Check if the key represents a valid instantiation.
    bool isValid() const {
        return decl != nullptr && typeArgs.size() > 0;
    }

    /// @brief Get the number of type arguments.
    size_t argCount() const {
        return typeArgs.size();
    }

    /// @brief Get a type argument by index.
    TypeAST* getArg(size_t index) const {
        if (index < typeArgs.size()) {
            return typeArgs[index];
        }
        return nullptr;
    }
};

/// @brief Hash for GenericInstantiationKey.
struct GenericInstantiationKeyHash {
    size_t operator()(const GenericInstantiationKey& key) const {
        size_t h1 = std::hash<DeclAST*>{}(key.decl);
        size_t h2 = 0;
        for (size_t i = 0; i < key.typeArgs.size(); ++i) {
            h2 ^= std::hash<TypeAST*>{}(key.typeArgs[i]) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
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
public:
    // ─── Type Aliases ──────────────────────────────────────────────────────

    using FunctionInstantiationMap = std::unordered_map<
        GenericInstantiationKey,
        llvm::Function*,
        GenericInstantiationKeyHash
    >;

    using StructInstantiationMap = std::unordered_map<
        GenericInstantiationKey,
        llvm::Type*,
        GenericInstantiationKeyHash
    >;

    // ─── Function Instantiations ──────────────────────────────────────────

    /// @brief Map from generic function declaration to its instantiations.
    std::unordered_map<FuncDeclAST*, FunctionInstantiationMap> functionInstantiations;

    /// @brief Map from generic struct declaration to its instantiations.
    std::unordered_map<StructDeclAST*, StructInstantiationMap> structInstantiations;

    // ─── Query Methods ─────────────────────────────────────────────────────

    /// @brief Check if a function instantiation already exists.
    bool hasFunctionInstantiation(
        FuncDeclAST* decl,
        const ArenaSpan<TypeAST*>& typeArgs
    ) const {
        auto it = functionInstantiations.find(decl);
        if (it == functionInstantiations.end()) return false;

        GenericInstantiationKey key{decl, typeArgs};
        return it->second.find(key) != it->second.end();
    }

    /// @brief Get a function instantiation if it exists.
    llvm::Function* getFunctionInstantiation(
        FuncDeclAST* decl,
        const ArenaSpan<TypeAST*>& typeArgs
    ) const {
        auto it = functionInstantiations.find(decl);
        if (it == functionInstantiations.end()) return nullptr;

        GenericInstantiationKey key{decl, typeArgs};
        auto found = it->second.find(key);
        return found != it->second.end() ? found->second : nullptr;
    }

    /// @brief Store a function instantiation.
    void storeFunctionInstantiation(
        FuncDeclAST* decl,
        const ArenaSpan<TypeAST*>& typeArgs,
        llvm::Function* func
    ) {
        GenericInstantiationKey key{decl, typeArgs};
        functionInstantiations[decl][key] = func;
    }

    /// @brief Check if a struct instantiation already exists.
    bool hasStructInstantiation(
        StructDeclAST* decl,
        const ArenaSpan<TypeAST*>& typeArgs
    ) const {
        auto it = structInstantiations.find(decl);
        if (it == structInstantiations.end()) return false;

        GenericInstantiationKey key{decl, typeArgs};
        return it->second.find(key) != it->second.end();
    }

    /// @brief Get a struct instantiation if it exists.
    llvm::Type* getStructInstantiation(
        StructDeclAST* decl,
        const ArenaSpan<TypeAST*>& typeArgs
    ) const {
        auto it = structInstantiations.find(decl);
        if (it == structInstantiations.end()) return nullptr;

        GenericInstantiationKey key{decl, typeArgs};
        auto found = it->second.find(key);
        return found != it->second.end() ? found->second : nullptr;
    }

    /// @brief Store a struct instantiation.
    void storeStructInstantiation(
        StructDeclAST* decl,
        const ArenaSpan<TypeAST*>& typeArgs,
        llvm::Type* type
    ) {
        GenericInstantiationKey key{decl, typeArgs};
        structInstantiations[decl][key] = type;
    }

    // ─── Bulk Operations ──────────────────────────────────────────────────

    /// @brief Get all function instantiations for a declaration.
    /// @return A const reference to the instantiation map, or empty if none.
    const FunctionInstantiationMap& getFunctionInstantiations(
        FuncDeclAST* decl
    ) const {
        static const FunctionInstantiationMap empty;
        auto it = functionInstantiations.find(decl);
        return it != functionInstantiations.end() ? it->second : empty;
    }

    /// @brief Get all struct instantiations for a declaration.
    const StructInstantiationMap& getStructInstantiations(
        StructDeclAST* decl
    ) const {
        static const StructInstantiationMap empty;
        auto it = structInstantiations.find(decl);
        return it != structInstantiations.end() ? it->second : empty;
    }

    /// @brief Get the number of function instantiations.
    size_t functionInstantiationCount() const {
        size_t count = 0;
        for (const auto& pair : functionInstantiations) {
            count += pair.second.size();
        }
        return count;
    }

    /// @brief Get the number of struct instantiations.
    size_t structInstantiationCount() const {
        size_t count = 0;
        for (const auto& pair : structInstantiations) {
            count += pair.second.size();
        }
        return count;
    }

    // ─── Lifecycle ─────────────────────────────────────────────────────────

    /// @brief Clear all instantiations (for hot-reload).
    void clear() {
        functionInstantiations.clear();
        structInstantiations.clear();
    }

    /// @brief Check if the registry is empty.
    bool empty() const {
        return functionInstantiations.empty() && structInstantiations.empty();
    }

    /// @brief Get total number of instantiations (functions + structs).
    size_t totalCount() const {
        return functionInstantiationCount() + structInstantiationCount();
    }
};

} // namespace codegen