/// @file execution/ModuleLoader.hpp
/// @brief Module loading functions - internal implementation.

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

// ─── Core Loading Functions ─────────────────────────────────────────────

/// @brief Load or reload modules into the interpreter.
bool loadOrReloadModules(InterpreterContext& ctx, 
                         const std::vector<ModuleAST*>& modules,
                         bool isHotReload = false);

/// @brief Load a single module (convenience wrapper).
bool loadModule(InterpreterContext& ctx, ModuleAST* module);

/// @brief Load multiple modules (convenience wrapper).
bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules);

/// @brief Hot-reload a module and all affected modules.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     InternedString name);

/// @brief Hot-reload with string name.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     const std::string& name);

/// @brief Check if a module is loaded.
bool isModuleLoaded(const InterpreterContext& ctx, InternedString name);

/// @brief Get the active module.
ModuleAST* getActiveModule(const InterpreterContext& ctx);

// ─── Internal Helpers ───────────────────────────────────────────────────

/// @brief Generate a unique module name from an AST.
InternedString generateModuleName(InterpreterContext& ctx, ModuleAST* module);

/// @brief Lower a single module to LLVM IR.
std::unique_ptr<llvm::Module> lowerModule(InterpreterContext& ctx, ModuleAST* module);

/// @brief Lower multiple modules to separate LLVM IR modules.
std::vector<std::unique_ptr<llvm::Module>> lowerModulesSeparately(
    InterpreterContext& ctx,
    const std::vector<ModuleAST*>& modules);

/// @brief Extract module dependencies from AST.
std::vector<InternedString> extractModuleDependencies(
    InterpreterContext& ctx,
    ModuleAST* module);

/// @brief Register foreign libraries from modules.
void registerModuleLibraries(InterpreterContext& ctx, 
                            const std::vector<ModuleAST*>& modules);

} // namespace interpreter