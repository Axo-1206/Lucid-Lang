/// @file registry/AttributeRegistry.hpp
/// @brief Pure data registry for attributes - no semantic or codegen dependencies.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <vector>

namespace sema {

/// @brief Information about a registered attribute.
struct AttributeInfo {
    InternedString name;
    bool canHaveArgs = false;
    bool requiresStringArgs = true;
    size_t minArgs = 0;
    size_t maxArgs = 0;
    bool appliesToGenericOnly = false;  // For the generic-specific validation
    std::unordered_set<ASTKind> allowedKinds;  // set of allowed declaration kinds
    
    AttributeInfo() = default;
    AttributeInfo(InternedString n, bool hasArgs, bool reqStrArgs,
                  size_t min, size_t max, bool genericOnly,
                  std::initializer_list<ASTKind> kinds)
        : name(n), canHaveArgs(hasArgs), requiresStringArgs(reqStrArgs),
          minArgs(min), maxArgs(max), appliesToGenericOnly(genericOnly),
          allowedKinds(kinds) {}
};

/// @brief Attribute entry for the data table.
struct AttributeEntry {
    std::string_view name;
    bool canHaveArgs;
    bool requiresStringArgs;
    size_t minArgs;
    size_t maxArgs;
    std::initializer_list<ASTKind> allowedKinds;  // which declarations this attribute can attach to
};

/// @brief Core attribute registry - pure data, no semantic dependencies.
class AttributeRegistry {
public:
    static AttributeRegistry& getInstance(StringPool& pool);

    const AttributeInfo* getInfo(InternedString name) const;
    bool canHaveArgs(InternedString name) const;
    size_t getMinArgs(InternedString name) const;
    size_t getMaxArgs(InternedString name) const;
    bool requiresStringArgs(InternedString name) const;
    bool appliesToGenericOnly(InternedString name) const;

    /// @brief Check if an attribute can be applied to a declaration kind.
    bool isAllowedOnDecl(InternedString attrName, ASTKind declKind) const {
        auto* info = getInfo(attrName);
        if (!info) return false;
        return info->allowedKinds.find(declKind) != info->allowedKinds.end();
    }

    std::vector<InternedString> getAllNames() const;
    std::vector<std::string> getAllNamesAsStrings() const;

private:
    AttributeRegistry(StringPool& pool);
    ~AttributeRegistry() = default;

    AttributeRegistry(const AttributeRegistry&) = delete;
    AttributeRegistry& operator=(const AttributeRegistry&) = delete;

    StringPool& m_pool;
    std::unordered_map<InternedString, AttributeInfo> m_attributes;
};

} // namespace sema