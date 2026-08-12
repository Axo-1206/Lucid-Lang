/// @file core/ModuleRegistry.hpp
/// @brief Tracks loaded modules and their versions.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <unordered_map>
#include <vector>
#include <string>

namespace interpreter {

/// @brief Information about a loaded module.
struct ModuleInfo {
    InternedString name;
    ModuleAST* ast = nullptr;
    uint64_t version = 0;
    bool isActive = true;

    /// @brief Get the fully qualified name with version.
    InternedString getFullName() const;
};

/// @brief Registry for tracking loaded modules.
class ModuleRegistry {
public:
    explicit ModuleRegistry(StringPool& pool);
    ~ModuleRegistry() = default;

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

    /// @brief Get module info by name (const).
    const ModuleInfo* getModuleInfo(InternedString name) const;

    /// @brief Check if a module is loaded.
    bool hasModule(InternedString name) const;

    /// @brief Get the active module.
    /// @return The active module, or nullptr if none.
    ModuleInfo* getActiveModule();

    /// @brief Set the active module.
    void setActiveModule(InternedString name);

    /// @brief Get all loaded modules.
    std::vector<ModuleInfo*> getAllModules();

    /// @brief Get all loaded modules (const).
    std::vector<const ModuleInfo*> getAllModules() const;

    /// @brief Get the module count.
    size_t count() const { return m_modules.size(); }

    /// @brief Clear all modules.
    void clear();

    /// @brief Increment the version for a module.
    uint64_t incrementVersion(InternedString name);

private:
    StringPool& m_pool;
    std::unordered_map<uint32_t, ModuleInfo> m_modules;
    InternedString m_activeModuleName;
};

} // namespace interpreter