/// @file core/ModuleRegistry.cpp
/// @brief Implementation of the module registry - NO VERSIONS.

#include "ModuleRegistry.hpp"

#include <algorithm>
#include <stdexcept>
#include <queue>
#include <set>
#include <functional>

namespace interpreter {

// ─── ModuleInfo ─────────────────────────────────────────────────────────

bool ModuleInfo::dependsOn(InternedString other) const {
    for (const auto& dep : dependencies) {
        if (dep == other) return true;
    }
    return false;
}

bool ModuleInfo::isDependencyOf(InternedString other) const {
    return dependents.find(other) != dependents.end();
}

// ─── ModuleRegistry ─────────────────────────────────────────────────────

ModuleRegistry::ModuleRegistry(StringPool& pool)
    : m_pool(pool) {
}

ModuleInfo& ModuleRegistry::registerModule(InternedString name, ModuleAST* ast) {
    if (!name.isValid()) {
        throw std::invalid_argument("Cannot register module with invalid name");
    }

    if (ast == nullptr) {
        throw std::invalid_argument("Cannot register module with null AST");
    }

    auto it = m_modules.find(name.id);
    if (it != m_modules.end()) {
        // ─── Update existing module ─────────────────────────────────────
        // Keep the same name and dependencies, just update the AST.
        // This is what happens on hot-reload.
        it->second.ast = ast;
        it->second.isActive = true;
        return it->second;
    }

    // ─── Create new module entry ──────────────────────────────────────
    ModuleInfo info;
    info.name = name;
    info.ast = ast;
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

    // Remove from dependents lists of dependencies
    for (const auto& dep : it->second.dependencies) {
        auto depIt = m_modules.find(dep.id);
        if (depIt != m_modules.end()) {
            depIt->second.dependents.erase(name);
        }
    }

    if (m_activeModuleName.isValid() && m_activeModuleName.id == name.id) {
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
        m_activeModuleName = InternedString();
        return;
    }

    if (!hasModule(name)) {
        throw std::runtime_error("Cannot set active module: module not found");
    }

    // Mark all modules as inactive
    for (auto& pair : m_modules) {
        pair.second.isActive = false;
    }

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
    m_activeModuleName = InternedString();
}

void ModuleRegistry::setDependencies(InternedString name, 
                                     const std::vector<InternedString>& deps) {
    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        throw std::runtime_error("Cannot set dependencies: module not found");
    }

    // ─── 1. Remove old dependencies ──────────────────────────────────────
    for (const auto& oldDep : it->second.dependencies) {
        auto depIt = m_modules.find(oldDep.id);
        if (depIt != m_modules.end()) {
            depIt->second.dependents.erase(name);
        }
    }

    // ─── 2. Set new dependencies ─────────────────────────────────────────
    it->second.dependencies = deps;
    for (const auto& dep : deps) {
        auto depIt = m_modules.find(dep.id);
        if (depIt == m_modules.end()) {
            throw std::runtime_error("Dependency module not found: " + 
                                     m_pool.lookup(dep));
        }
        depIt->second.dependents.insert(name);
    }

    // ─── 3. Validate the graph ───────────────────────────────────────────
    updateDependencyGraph(name);
}

std::vector<ModuleInfo*> ModuleRegistry::getDependents(InternedString name) {
    std::vector<ModuleInfo*> result;
    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        return result;
    }

    for (const auto& depName : it->second.dependents) {
        auto depIt = m_modules.find(depName.id);
        if (depIt != m_modules.end()) {
            result.push_back(&depIt->second);
        }
    }
    return result;
}

std::vector<const ModuleInfo*> ModuleRegistry::getDependents(InternedString name) const {
    std::vector<const ModuleInfo*> result;
    auto it = m_modules.find(name.id);
    if (it == m_modules.end()) {
        return result;
    }

    for (const auto& depName : it->second.dependents) {
        auto depIt = m_modules.find(depName.id);
        if (depIt != m_modules.end()) {
            result.push_back(&depIt->second);
        }
    }
    return result;
}

bool ModuleRegistry::hasErrorModules() const {
    for (const auto& pair : m_modules) {
        if (pair.second.ast && pair.second.ast->hasErrors) {
            return true;
        }
    }
    return false;
}

std::vector<ModuleInfo*> ModuleRegistry::getAffectedModules(InternedString changedModule) {
    std::vector<ModuleInfo*> result;
    
    if (!hasModule(changedModule)) {
        return result;
    }

    // BFS to find all modules that depend on the changed module
    std::queue<InternedString> queue;
    std::set<uint32_t> visited;
    
    queue.push(changedModule);
    visited.insert(changedModule.id);

    while (!queue.empty()) {
        InternedString current = queue.front();
        queue.pop();

        auto info = getModuleInfo(current);
        if (!info) continue;

        // Add this module to results (except the original)
        if (current.id != changedModule.id) {
            result.push_back(info);
        }

        // Add all dependents to queue
        for (const auto& depName : info->dependents) {
            if (visited.find(depName.id) == visited.end()) {
                visited.insert(depName.id);
                queue.push(depName);
            }
        }
    }

    return result;
}

void ModuleRegistry::updateDependencyGraph(InternedString name) {
    // ─── 1. Fix any inconsistencies ──────────────────────────────────────
    for (const auto& pair : m_modules) {
        const ModuleInfo& info = pair.second;
        
        for (const auto& dep : info.dependencies) {
            auto depIt = m_modules.find(dep.id);
            if (depIt == m_modules.end()) {
                throw std::runtime_error("Dependency graph corruption: missing module " +
                                         m_pool.lookup(dep));
            }
            // Ensure reverse link exists
            if (depIt->second.dependents.find(info.name) == depIt->second.dependents.end()) {
                depIt->second.dependents.insert(info.name);
            }
        }
        
        for (const auto& depName : info.dependents) {
            auto depIt = m_modules.find(depName.id);
            if (depIt == m_modules.end()) {
                throw std::runtime_error("Dependency graph corruption: missing dependent " +
                                         m_pool.lookup(depName));
            }
            // Ensure forward link exists
            bool found = false;
            for (const auto& dep : depIt->second.dependencies) {
                if (dep == info.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                depIt->second.dependencies.push_back(info.name);
            }
        }
    }
    
    // ─── 2. Detect cycles ──────────────────────────────────────────────
    validateDependencyGraph();
}

void ModuleRegistry::validateDependencyGraph() const {
    // DFS-based cycle detection
    std::set<uint32_t> visited;
    std::set<uint32_t> recursionStack;

    std::function<bool(uint32_t)> hasCycle = [&](uint32_t id) -> bool {
        if (recursionStack.find(id) != recursionStack.end()) {
            return true;
        }
        if (visited.find(id) != visited.end()) {
            return false;
        }

        visited.insert(id);
        recursionStack.insert(id);

        auto it = m_modules.find(id);
        if (it != m_modules.end()) {
            for (const auto& dep : it->second.dependencies) {
                if (hasCycle(dep.id)) {
                    return true;
                }
            }
        }

        recursionStack.erase(id);
        return false;
    };

    for (const auto& pair : m_modules) {
        if (hasCycle(pair.first)) {
            throw std::runtime_error("Cyclic dependency detected in module graph");
        }
    }
}

} // namespace interpreter