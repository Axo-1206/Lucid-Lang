/// @file sema/context/typeIdRegistry.hpp
/// @brief Registry for mapping concrete types to runtime type id.
///
/// This registry is used for type-erased generic dispatch. It assigns a unique
/// 32-bit id to each concrete type that appears in a type-erased generic context.
///
/// ─── Key Design ──────────────────────────────────────────────────────────────
/// 1. **Compile-time Registry**: Id are assigned during semantic analysis.
///    The registry lives in SemaContext and is populated as generic types are
///    instantiated.
///
/// 2. **Type Deduplication**: The same concrete type always gets the same id.
///    This is achieved by keying on TypeAST* (which are canonicalized by the
///    TypeCache in SemaContext).
///
/// 3. **Runtime Dispatch**: Id are embedded in generated code (e.g., as
///    constants in IdgedSlot structs) and used for runtime type dispatch in
///    type-erased generic functions.
///
/// 4. **Id 0 is Reserved**: Id 0 means "no id" (non-generic or specialized).
///
/// ─── Usage Example ──────────────────────────────────────────────────────────
/// @code
///   // In resolveNamedType (type-erased path)
///   TypeAST* canonicalType = ctx.getNamedType("Box", {intType});
///   type->typeId = ctx.gettypeIdRegistry().getId(canonicalType);
///
///   // In resolveCallExpr
///   uint32_t id = ctx.gettypeIdRegistry().getId(argType);
///   call->typeIds.push_back(id);
/// @endcode

#pragma once

#include "core/ast/TypeAST.hpp"

#include <unordered_map>
#include <cstdint>

namespace sema {

/// @brief Registry for mapping types to runtime id.
///
/// This is a compile-time registry that assigns a unique id to each
/// concrete type that appears in a type-erased generic context.
///
/// @note The registry is intended to be owned by SemaContext and accessed
///       via SemaContext::getTypeIdRegistry().
struct TypeIdRegistry {
    /// Map from type AST to id.
    std::unordered_map<TypeAST*, uint32_t> typeToId;

    /// Map from id to type AST (for debugging).
    std::unordered_map<uint32_t, TypeAST*> idToType;

    /// The next available id. Starts at 1 because 0 is reserved.
    uint32_t nextId = 1;

    /// @brief Get the id for a type, assigning a new one if not present.
    /// @param type The type to get a id for (must be non-null).
    /// @return A unique 32-bit id for this type.
    uint32_t getId(TypeAST* type) {
        auto it = typeToId.find(type);
        if (it != typeToId.end()) {
            return it->second;
        }
        uint32_t id = nextId++;
        typeToId[type] = id;
        idToType[id] = type;
        return id;
    }

    /// @brief Get the type for a id (for debugging).
    /// @param id The id to look up.
    /// @return The type AST, or nullptr if the id is not found.
    TypeAST* getType(uint32_t id) const {
        auto it = idToType.find(id);
        return it != idToType.end() ? it->second : nullptr;
    }

    /// @brief Check if a type has a id.
    bool hasId(TypeAST* type) const {
        return typeToId.find(type) != typeToId.end();
    }

    /// @brief Get the id for a type without assigning a new one.
    /// @param type The type to look up.
    /// @return The id, or 0 if the type is not registered.
    uint32_t findId(TypeAST* type) const {
        auto it = typeToId.find(type);
        return it != typeToId.end() ? it->second : 0;
    }

    /// @brief Clear all id.
    void clear() {
        typeToId.clear();
        idToType.clear();
        nextId = 1;
    }

    /// @brief Get the number of registered types.
    size_t size() const {
        return typeToId.size();
    }

    /// @brief Check if the registry is empty.
    bool empty() const {
        return typeToId.empty();
    }
};

} // namespace sema