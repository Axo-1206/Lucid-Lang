/// @file execution/ModuleLoader.hpp
/// @brief Module loading functions - procedural style.

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

// ─── Module Loading ──────────────────────────────────────────────────────

/// @brief Load a single module into the interpreter.
/// @param ctx The interpreter context.
/// @param module The AST module to load.
/// @return true on success.
/// @throws InterpreterError if loading fails.
bool loadModule(InterpreterContext& ctx, ModuleAST* module);

/// @brief Load multiple modules (in dependency order).
/// @param ctx The interpreter context.
/// @param modules The AST modules to load.
/// @return true on success.
/// @throws InterpreterError if loading fails.
bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules);

/// @brief Check if a module is loaded by name.
/// @param ctx The interpreter context.
/// @param name The module name.
/// @return true if the module is loaded.
bool isModuleLoaded(const InterpreterContext& ctx, InternedString name);

/// @brief Get the active module.
/// @param ctx The interpreter context.
/// @return The active module AST, or nullptr if none.
ModuleAST* getActiveModule(const InterpreterContext& ctx);

// ─── Internal Helpers ────────────────────────────────────────────────────

/// @brief Generate a unique module name from an AST.
/// @param ctx The interpreter context.
/// @param module The module AST.
/// @return The generated name.
InternedString generateModuleName(InterpreterContext& ctx, ModuleAST* module);

/// @brief Lower a single module to LLVM IR.
/// @param ctx The interpreter context.
/// @param module The AST module.
/// @return The LLVM module.
/// @throws InterpreterError if lowering fails.
std::unique_ptr<llvm::Module> lowerModule(InterpreterContext& ctx, ModuleAST* module);

/// @brief Lower multiple modules to a single LLVM IR module.
/// @param ctx The interpreter context.
/// @param modules The AST modules.
/// @param moduleName The name for the combined module.
/// @return The LLVM module.
/// @throws InterpreterError if lowering fails.
std::unique_ptr<llvm::Module> lowerModules(
    InterpreterContext& ctx,
    const std::vector<ModuleAST*>& modules,
    InternedString moduleName
);

/// @brief Check if any module has errors.
bool hasErrors(const std::vector<ModuleAST*>& modules);

/// @brief Report errors from modules.
void reportErrors(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules);

} // namespace interpreter