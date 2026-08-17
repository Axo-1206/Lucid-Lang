/// @file core/ModuleRegistry.cpp
/// @brief Implementation of the module registry.

#include "ModuleRegistry.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace interpreter {

// ─── ModuleInfo ─────────────────────────────────────────────────────────

InternedString ModuleInfo::getFullName() const {
    if (version == 0) {
        return name;
    }
    
    // Construct versioned name: "name_v123"
    // Note: In a real implementation, you'd want to intern this string
    // or compute it lazily. For now, we return the base name since we
    // can't create new InternedStrings without a StringPool reference.
    return name;
}

// ─── ModuleRegistry ─────────────────────────────────────────────────────

ModuleRegistry::ModuleRegistry(StringPool& pool)
    : m_pool(pool) {
    // m_activeModuleName default-constructs to invalid (id=0)
}

ModuleInfo& ModuleRegistry::registerModule(InternedString name, ModuleAST* ast) {
    if (!name.isValid()) {
        throw std::invalid_argument("Cannot register module with invalid name");
    }

    if (ast == nullptr) {
        throw std::invalid_argument("Cannot register module with null AST");
    }

    // Check if module already exists
    auto it = m_modules.find(name.id);
    if (it != m_modules.end()) {
        // Update existing module
        it->second.ast = ast;
        it->second.isActive = true;
        // Version is incremented separately via incrementVersion()
        return it->second;
    }

    // Create new module entry
    ModuleInfo info;
    info.name = name;
    info.ast = ast;
    info.version = 0;
    info.isActive = true;

    auto result = m_modules.emplace(name.id, info);
    return result.first->second;
}

bool ModuleRegistry::unregisterModule(InternedString name) {
    if (!name.isValid()) {
        return false;
    }

    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        return false;
    }

    // If this was the active module, clear the active flag
    if (m_activeModuleName.isValid() && m_activeModuleName.id == name.id) {
        // Reset to invalid by default-constructing
        m_activeModuleName = InternedString();
    }

    m_modules.erase(it);
    return true;
}

ModuleInfo* ModuleRegistry::getModuleInfo(InternedString name) {
    if (!name.isValid()) {
        return nullptr;
    }

    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        return nullptr;
    }

    return &it->second;
}

const ModuleInfo* ModuleRegistry::getModuleInfo(InternedString name) const {
    if (!name.isValid()) {
        return nullptr;
    }

    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        return nullptr;
    }

    return &it->second;
}

bool ModuleRegistry::hasModule(InternedString name) const {
    if (!name.isValid()) {
        return false;
    }
    return m_modules.find(name.id) != m_modules.end();
}

ModuleInfo* ModuleRegistry::getActiveModule() {
    if (!m_activeModuleName.isValid()) {
        return nullptr;
    }

    return getModuleInfo(m_activeModuleName);
}

const ModuleInfo* ModuleRegistry::getActiveModule() const {
    if (!m_activeModuleName.isValid()) {
        return nullptr;
    }

    return getModuleInfo(m_activeModuleName);
}

void ModuleRegistry::setActiveModule(InternedString name) {
    if (!name.isValid()) {
        // Clear active module
        m_activeModuleName = InternedString();
        return;
    }

    // Verify the module exists
    if (!hasModule(name)) {
        throw std::runtime_error("Cannot set active module: module not found");
    }

    // Mark all modules as inactive
    for (auto& pair : m_modules) {
        pair.second.isActive = false;
    }

    // Mark the specified module as active
    auto it = m_modules.find(name.id);
    if (it != m_modules.end()) {
        it->second.isActive = true;
        m_activeModuleName = name;
    }
}

std::vector<ModuleInfo*> ModuleRegistry::getAllModules() {
    std::vector<ModuleInfo*> result;
    result.reserve(m_modules.size());

    for (auto& pair : m_modules) {
        result.push_back(&pair.second);
    }

    return result;
}

std::vector<const ModuleInfo*> ModuleRegistry::getAllModules() const {
    std::vector<const ModuleInfo*> result;
    result.reserve(m_modules.size());

    for (const auto& pair : m_modules) {
        result.push_back(&pair.second);
    }

    return result;
}

void ModuleRegistry::clear() {
    m_modules.clear();
    m_activeModuleName = InternedString(); // Reset to invalid
}

uint64_t ModuleRegistry::incrementVersion(InternedString name) {
    if (!name.isValid()) {
        throw std::invalid_argument("Cannot increment version for invalid name");
    }

    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        throw std::runtime_error("Cannot increment version: module not found");
    }

    return ++it->second.version;
}

} // namespace interpreter