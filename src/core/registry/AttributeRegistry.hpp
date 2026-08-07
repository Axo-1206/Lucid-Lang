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
    
    AttributeInfo() = default;
    AttributeInfo(InternedString n, bool hasArgs = false, 
                  size_t min = 0, size_t max = 0)
        : name(n), canHaveArgs(hasArgs), requiresStringArgs(true), 
          minArgs(min), maxArgs(max) {}
};

/// @brief Attribute entry for the data table.
struct AttributeEntry {
    std::string_view name;
    bool canHaveArgs;
    bool requiresStringArgs;
    size_t minArgs;
    size_t maxArgs;
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

    std::vector<InternedString> getAllNames() const;
    std::vector<std::string> getAllNamesAsStrings() const;

    bool isValidForDecl(InternedString attrName, const DeclAST* decl) const;

private:
    AttributeRegistry(StringPool& pool);
    ~AttributeRegistry() = default;

    AttributeRegistry(const AttributeRegistry&) = delete;
    AttributeRegistry& operator=(const AttributeRegistry&) = delete;

    StringPool& m_pool;
    std::unordered_map<InternedString, AttributeInfo> m_attributes;
    bool m_initialized = false;
};

} // namespace sema