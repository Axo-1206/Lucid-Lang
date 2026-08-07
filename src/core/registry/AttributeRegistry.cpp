/// @file registry/AttributeRegistry.cpp
/// @brief Implementation of attribute registry.

#include "AttributeRegistry.hpp"
#include "core/memory/StringPool.hpp"
#include <algorithm>

namespace sema {

// ─── Data Table ─────────────────────────────────────────────────────────────

// AttributeEntry is defined at namespace scope in AttributeRegistry.hpp
static const AttributeEntry ATTRIBUTE_TABLE[] = {
    {"export",     false, true, 0, 0},
    {"foreign",    true,  true, 1, 1},
    {"link",       true,  true, 1, 0},  // 1+ args, unbounded (maxArgs=0 means no limit)
    {"deprecated", true,  true, 0, 1},
    {"inline",     false, true, 0, 0},
    {"noinline",   false, true, 0, 0},
};

static constexpr size_t ATTRIBUTE_COUNT = sizeof(ATTRIBUTE_TABLE) / sizeof(ATTRIBUTE_TABLE[0]);

// ─── Singleton ──────────────────────────────────────────────────────────────

AttributeRegistry& AttributeRegistry::getInstance(StringPool& pool) {
    static AttributeRegistry instance(pool);
    return instance;
}

// ─── Constructor ────────────────────────────────────────────────────────────

AttributeRegistry::AttributeRegistry(StringPool& pool) : m_pool(pool) {
    for (const auto& entry : ATTRIBUTE_TABLE) {
        InternedString name = m_pool.intern(entry.name);
        m_attributes[name] = AttributeInfo(
            name, entry.canHaveArgs, entry.minArgs, entry.maxArgs
        );
    }
}

// ─── Query Methods ─────────────────────────────────────────────────────────

const AttributeInfo* AttributeRegistry::getInfo(InternedString name) const {
    auto it = m_attributes.find(name);
    if (it != m_attributes.end()) {
        return &it->second;
    }
    return nullptr;
}

bool AttributeRegistry::canHaveArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->canHaveArgs : false;
}

size_t AttributeRegistry::getMinArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->minArgs : 0;
}

size_t AttributeRegistry::getMaxArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->maxArgs : 0;
}

bool AttributeRegistry::requiresStringArgs(InternedString name) const {
    auto* info = getInfo(name);
    return info ? info->requiresStringArgs : false;
}

std::vector<InternedString> AttributeRegistry::getAllNames() const {
    std::vector<InternedString> names;
    names.reserve(m_attributes.size());
    for (const auto& pair : m_attributes) {
        names.push_back(pair.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> AttributeRegistry::getAllNamesAsStrings() const {
    std::vector<std::string> names;
    names.reserve(m_attributes.size());
    for (const auto& pair : m_attributes) {
        names.push_back(m_pool.lookup(pair.first));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool AttributeRegistry::isValidForDecl(InternedString attrName, const DeclAST* decl) const {
    if (!decl) return false;
    
    // @[export] only valid at module level
    if (m_pool.lookupView(attrName) == "export") {
        // This requires SemaContext to check isAtModuleLevel()
        // So this is a partial check - semantic validation handles the rest
        return true;
    }
    
    // @[foreign], @[link] and @[inline] only valid on functions
    if (m_pool.lookupView(attrName) == "foreign" || 
        m_pool.lookupView(attrName) == "inline" ||
         m_pool.lookupView(attrName) == "link" ||
        m_pool.lookupView(attrName) == "noinline") {
        return decl->isa<FuncDeclAST>();
    }
    
    // @[deprecated] valid on most declarations
    return true;
}

} // namespace sema