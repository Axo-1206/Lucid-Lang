/// @file execution/ModuleLoader.hpp
/// @brief Loads AST modules into the interpreter.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../core/InterpreterContext.hpp"
#include <memory>
#include <vector>

namespace llvm {
class Module;
}

namespace interpreter {

/// @brief Loads modules from AST to LLVM IR.
class ModuleLoader {
public:
    explicit ModuleLoader(InterpreterContext& ctx);
    ~ModuleLoader() = default;

    /// @brief Load a single module.
    /// @param module The AST module to load.
    /// @return true on success.
    /// @throws InterpreterError if loading fails.
    bool loadModule(ModuleAST* module);

    /// @brief Load multiple modules (in dependency order).
    /// @param modules The AST modules to load.
    /// @return true on success.
    /// @throws InterpreterError if loading fails.
    bool loadModules(const std::vector<ModuleAST*>& modules);

    /// @brief Check if a module is loaded.
    bool isLoaded(InternedString name) const;

    /// @brief Get the active module.
    ModuleAST* getActiveModule() const;

private:
    InterpreterContext& m_ctx;

    /// @brief Lower AST to LLVM IR.
    std::unique_ptr<llvm::Module> lowerModule(ModuleAST* module);

    /// @brief Lower multiple ASTs to a single LLVM module.
    std::unique_ptr<llvm::Module> lowerModules(
        const std::vector<ModuleAST*>& modules,
        InternedString moduleName
    );

    /// @brief Generate a unique name for a module.
    InternedString generateModuleName(ModuleAST* module);

    /// @brief Check if any module has errors.
    bool hasErrors(const std::vector<ModuleAST*>& modules) const;

    /// @brief Report errors from modules.
    void reportErrors(const std::vector<ModuleAST*>& modules) const;
};

} // namespace interpreter