/// @file Interpreter.hpp
/// @brief Main interpreter API - procedural style.
///
/// This is the PUBLIC API for the interpreter. It orchestrates the
/// loading, compilation, and execution of Lucid modules.
/// All heavy lifting is delegated to specialized modules.

#pragma once

#include "core/InterpreterContext.hpp"
#include "core/ModuleRegistry.hpp"
#include "support/InterpreterOptions.hpp"
#include "support/ExecutionResult.hpp"

#include <vector>

namespace interpreter {

// ─── Initialization ─────────────────────────────────────────────────────

/// @brief Initialize the interpreter context.
void initialize(InterpreterContext& ctx, 
                const InterpreterOptions& options = InterpreterOptions{});

/// @brief Check if initialized.
bool isInitialized(const InterpreterContext& ctx);

// ─── Execution ──────────────────────────────────────────────────────────

/// @brief Run modules with optional hot-reload support.
/// @param ctx The interpreter context.
/// @param modules The AST modules to run (in dependency order).
/// @param entryPoint The entry point name.
/// @param isHotReload Whether this is a hot-reload operation.
/// @return Execution result.
/// @throws InterpreterError if execution fails.
ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint = InternedString(),
                           bool isHotReload = false);

/// @brief Run modules with string entry point.
ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           const std::string& entryPoint = "",
                           bool isHotReload = false);

/// @brief Run a single module.
ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          InternedString entryPoint = InternedString(),
                          bool isHotReload = false);

/// @brief Run a single module with string entry point.
ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          const std::string& entryPoint = "",
                          bool isHotReload = false);

// ─── Hot-Reload ─────────────────────────────────────────────────────────

/// @brief Hot-reload a module and all affected modules.
/// @param ctx The interpreter context.
/// @param module The updated AST module.
/// @param name The module name.
/// @return true on success.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     InternedString name);

/// @brief Hot-reload with string name.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     const std::string& name);

// ─── Convenience Accessors ─────────────────────────────────────────────

/// @brief Get the loaded modules (delegates to ModuleRegistry).
std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx);

/// @brief Get modules affected by a change (delegates to ModuleRegistry).
std::vector<ModuleInfo*> getAffectedModules(InterpreterContext& ctx, 
                                            InternedString changedModule);

} // namespace interpreter