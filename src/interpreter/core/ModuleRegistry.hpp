/// @file core/ModuleRegistry.hpp
/// @brief Tracks loaded modules and their versions.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <unordered_map>
#include <vector>
#include <string>
#include <set>

namespace interpreter {

/// @brief Information about a loaded module.
struct ModuleInfo {
    InternedString name;
    ModuleAST* ast = nullptr;
    uint64_t version = 0;
    bool isActive = true;
    std::vector<InternedString> dependencies;  // Modules this module depends on
    std::set<InternedString> dependents;       // Modules that depend on this

    /// @brief Get the fully qualified name with version.
    InternedString getFullName() const;
    
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

    /// @brief Register a loaded module.
    /// @param name The module name.
    /// @param ast The module AST.
    /// @return The module info.
    ModuleInfo& registerModule(InternedString name, ModuleAST* ast);

    /// @brief Unregister a module.
    /// @param name The module name.
    /// @return true if the module was found and removed.
    bool unregisterModule(InternedString name);

    /// @brief Get module info by name.
    /// @return Pointer to module info, or nullptr if not found.
    ModuleInfo* getModuleInfo(InternedString name);
    const ModuleInfo* getModuleInfo(InternedString name) const;

    /// @brief Check if a module is loaded.
    bool hasModule(InternedString name) const;

    /// @brief Get the active module.
    ModuleInfo* getActiveModule();
    const ModuleInfo* getActiveModule() const;

    /// @brief Set the active module.
    void setActiveModule(InternedString name);

    /// @brief Get all loaded modules.
    std::vector<ModuleInfo*> getAllModules();
    std::vector<const ModuleInfo*> getAllModules() const;

    /// @brief Get the module count.
    size_t count() const { return m_modules.size(); }

    /// @brief Clear all modules.
    void clear();

    /// @brief Increment the version for a module.
    uint64_t incrementVersion(InternedString name);
    
    /// @brief Get the version for a module.
    uint64_t getVersion(InternedString name) const;

    /// @brief Set dependencies for a module.
    /// @param name The module name.
    /// @param deps The dependency names.
    void setDependencies(InternedString name, const std::vector<InternedString>& deps);

    /// @brief Get all modules that depend on a given module.
    std::vector<ModuleInfo*> getDependents(InternedString name);
    std::vector<const ModuleInfo*> getDependents(InternedString name) const;

    /// @brief Check if there are any modules with errors.
    bool hasErrorModules() const;

    /// @brief Get modules that need to be reloaded when a dependency changes.
    std::vector<ModuleInfo*> getAffectedModules(InternedString changedModule);

private:
    StringPool& m_pool;
    std::unordered_map<uint32_t, ModuleInfo> m_modules;
    InternedString m_activeModuleName;
    
    void updateDependencyGraph(InternedString name);
    void validateDependencyGraph() const;
};

} // namespace interpreter