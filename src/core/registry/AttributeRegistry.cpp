/// @file registry/AttributeRegistry.cpp
/// @brief Implementation of attribute registry.

#include "AttributeRegistry.hpp"
#include "core/memory/StringPool.hpp"
#include <algorithm>

namespace sema {

// ─── Data Table ─────────────────────────────────────────────────────────────

// AttributeEntry is defined at namespace scope in AttributeRegistry.hpp
static const AttributeEntry ATTRIBUTE_TABLE[] = {
    // Export works on any top-level declaration
    {"export",     false, true, 0, 0, {
        ASTKind::FuncDecl,
        ASTKind::StructDecl,
        ASTKind::EnumDecl,
        ASTKind::TraitDecl,
        ASTKind::VarDecl,
        ASTKind::ImportDecl
    }},
    
    // Foreign only on functions
    {"foreign",    true,  true, 1, 1, {
        ASTKind::FuncDecl
    }},
    
    // Link on functions or module-level
    {"link",       true,  true, 1, 0, {
        ASTKind::FuncDecl,
        ASTKind::ImportDecl
    }},
    
    // Deprecated on most declarations
    {"deprecated", true,  true, 0, 1, {
        ASTKind::FuncDecl,
        ASTKind::StructDecl,
        ASTKind::EnumDecl,
        ASTKind::TraitDecl,
        ASTKind::VarDecl,
        ASTKind::ImportDecl,
        ASTKind::FieldDecl,
        ASTKind::EnumVariant
    }},
    
    // Inline/Noinline only on functions
    {"inline",     false, true, 0, 0, {
        ASTKind::FuncDecl
    }},
    {"noinline",   false, true, 0, 0, {
        ASTKind::FuncDecl
    }},
    
    // Specialize on generic functions and generic structs
    {"specialize", false, true, 0, 0, {
        ASTKind::FuncDecl,
        ASTKind::StructDecl
    }},
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
            name,
            entry.canHaveArgs,
            entry.requiresStringArgs,
            entry.minArgs,
            entry.maxArgs,
            false,  // appliesToGenericOnly - handled separately in validator
            entry.allowedKinds
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

} // namespace sema