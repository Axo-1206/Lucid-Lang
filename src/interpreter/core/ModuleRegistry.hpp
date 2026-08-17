/// @file core/ModuleRegistry.hpp
/// @brief Track loaded modules and their dependencies - NO VERSIONS.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <unordered_map>
#include <vector>
#include <set>

namespace interpreter {

/// @brief Information about a loaded module - simple, no version.
struct ModuleInfo {
    InternedString name;                    // Unique identifier
    ModuleAST* ast = nullptr;              // Current AST (replaced on hot-reload)
    bool isActive = true;
    std::vector<InternedString> dependencies;  // Modules this depends on
    std::set<InternedString> dependents;       // Modules that depend on this

    /// @brief Check if this module depends on another.
    bool dependsOn(InternedString other) const;
    
    /// @brief Check if another module depends on this.
    bool isDependencyOf(InternedString other) const;
};

/// @brief Registry for tracking loaded modules.
class ModuleRegistry {
public:
    explicit ModuleRegistry(StringPool& pool);
    ~ModuleRegistry() = default;

    // Non-copyable
    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;

    /// @brief Register or update a module (same name = update).
    ModuleInfo& registerModule(InternedString name, ModuleAST* ast);

    /// @brief Unregister a module.
    bool unregisterModule(InternedString name);

    /// @brief Get module info by name.
    ModuleInfo* getModuleInfo(InternedString name);
    const ModuleInfo* getModuleInfo(InternedString name) const;

    /// @brief Check if a module is loaded.
    bool hasModule(InternedString name) const;

    /// @brief Get/set the active module.
    ModuleInfo* getActiveModule();
    const ModuleInfo* getActiveModule() const;
    void setActiveModule(InternedString name);

    /// @brief Get all loaded modules.
    std::vector<ModuleInfo*> getAllModules();
    std::vector<const ModuleInfo*> getAllModules() const;

    /// @brief Dependency management.
    void setDependencies(InternedString name, const std::vector<InternedString>& deps);
    std::vector<ModuleInfo*> getDependents(InternedString name);
    std::vector<const ModuleInfo*> getDependents(InternedString name) const;
    
    /// @brief Get all modules that depend on a changed module (transitive).
    std::vector<ModuleInfo*> getAffectedModules(InternedString changedModule);

    /// @brief Clear all modules.
    void clear();
    
    size_t count() const { return m_modules.size(); }
    bool hasErrorModules() const;

private:
    StringPool& m_pool;
    std::unordered_map<uint32_t, ModuleInfo> m_modules;
    InternedString m_activeModuleName;
    
    void updateDependencyGraph(InternedString name);
    void validateDependencyGraph() const;
};

} // namespace interpreter