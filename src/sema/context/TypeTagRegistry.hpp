/// @file sema/context/TypeTagRegistry.hpp
/// @brief Registry for mapping concrete types to runtime type tags.
///
/// This registry is used for type-erased generic dispatch. It assigns a unique
/// 32-bit tag to each concrete type that appears in a type-erased generic context.
///
/// ─── Key Design ──────────────────────────────────────────────────────────────
/// 1. **Compile-time Registry**: Tags are assigned during semantic analysis.
///    The registry lives in SemaContext and is populated as generic types are
///    instantiated.
///
/// 2. **Type Deduplication**: The same concrete type always gets the same tag.
///    This is achieved by keying on TypeAST* (which are canonicalized by the
///    TypeCache in SemaContext).
///
/// 3. **Runtime Dispatch**: Tags are embedded in generated code (e.g., as
///    constants in TaggedSlot structs) and used for runtime type dispatch in
///    type-erased generic functions.
///
/// 4. **Tag 0 is Reserved**: Tag 0 means "no tag" (non-generic or specialized).
///
/// ─── Usage Example ──────────────────────────────────────────────────────────
/// @code
///   // In resolveNamedType (type-erased path)
///   TypeAST* canonicalType = ctx.getNamedType("Box", {intType});
///   type->typeTag = ctx.getTypeTagRegistry().getTag(canonicalType);
///
///   // In resolveCallExpr
///   uint32_t tag = ctx.getTypeTagRegistry().getTag(argType);
///   call->typeTags.push_back(tag);
/// @endcode

#pragma once

#include <unordered_map>
#include <cstdint>

namespace sema {

// Forward declaration to avoid including TypeAST.hpp
struct TypeAST;

/// @brief Registry for mapping types to runtime tags.
///
/// This is a compile-time registry that assigns a unique tag to each
/// concrete type that appears in a type-erased generic context.
///
/// @note The registry is intended to be owned by SemaContext and accessed
///       via SemaContext::getTypeTagRegistry().
struct TypeTagRegistry {
    /// Map from type AST to tag.
    std::unordered_map<TypeAST*, uint32_t> typeToTag;

    /// Map from tag to type AST (for debugging).
    std::unordered_map<uint32_t, TypeAST*> tagToType;

    /// The next available tag. Starts at 1 because 0 is reserved.
    uint32_t nextTag = 1;

    /// @brief Get the tag for a type, assigning a new one if not present.
    /// @param type The type to get a tag for (must be non-null).
    /// @return A unique 32-bit tag for this type.
    uint32_t getTag(TypeAST* type) {
        auto it = typeToTag.find(type);
        if (it != typeToTag.end()) {
            return it->second;
        }
        uint32_t tag = nextTag++;
        typeToTag[type] = tag;
        tagToType[tag] = type;
        return tag;
    }

    /// @brief Get the type for a tag (for debugging).
    /// @param tag The tag to look up.
    /// @return The type AST, or nullptr if the tag is not found.
    TypeAST* getType(uint32_t tag) const {
        auto it = tagToType.find(tag);
        return it != tagToType.end() ? it->second : nullptr;
    }

    /// @brief Check if a type has a tag.
    bool hasTag(TypeAST* type) const {
        return typeToTag.find(type) != typeToTag.end();
    }

    /// @brief Get the tag for a type without assigning a new one.
    /// @param type The type to look up.
    /// @return The tag, or 0 if the type is not registered.
    uint32_t findTag(TypeAST* type) const {
        auto it = typeToTag.find(type);
        return it != typeToTag.end() ? it->second : 0;
    }

    /// @brief Clear all tags.
    void clear() {
        typeToTag.clear();
        tagToType.clear();
        nextTag = 1;
    }

    /// @brief Get the number of registered types.
    size_t size() const {
        return typeToTag.size();
    }

    /// @brief Check if the registry is empty.
    bool empty() const {
        return typeToTag.empty();
    }
};

} // namespace sema