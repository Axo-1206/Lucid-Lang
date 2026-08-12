/// @file Interpreter.hpp
/// @brief Main interpreter API - procedural style.

#pragma once

#include "core/InterpreterContext.hpp"
#include "core/ModuleRegistry.hpp"
#include "support/InterpreterOptions.hpp"
#include "support/ExecutionResult.hpp"

#include <vector>
#include <memory>

namespace interpreter {

// ─── Initialization ─────────────────────────────────────────────────────

/// @brief Initialize the interpreter context.
/// @param ctx The interpreter context.
/// @param options Configuration options.
/// @throws InterpreterError if initialization fails.
void initialize(InterpreterContext& ctx, const InterpreterOptions& options = InterpreterOptions{});

/// @brief Check if initialized.
bool isInitialized(const InterpreterContext& ctx);

// ─── Execution ──────────────────────────────────────────────────────────

/// @brief Run a single module.
/// @param ctx The interpreter context.
/// @param module The AST module to run.
/// @param entryPoint Override entry point (empty = use options).
/// @return Execution result.
/// @throws InterpreterError if execution fails.
ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          InternedString entryPoint = InternedString());

/// @brief Run a single module with string entry point.
ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          const std::string& entryPoint = "");

/// @brief Run multiple modules.
/// @param ctx The interpreter context.
/// @param modules The AST modules to run (in dependency order).
/// @param entryPoint Override entry point (empty = use options).
/// @return Execution result.
/// @throws InterpreterError if execution fails.
ExecutionResult runModules(InterpreterContext& ctx, 
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint = InternedString());

/// @brief Run multiple modules with string entry point.
ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           const std::string& entryPoint = "");

// ─── Loading ────────────────────────────────────────────────────────────

/// @brief Load a module without executing.
/// @param ctx The interpreter context.
/// @param module The AST module to load.
/// @return true on success.
/// @throws InterpreterError if loading fails.
bool loadModule(InterpreterContext& ctx, ModuleAST* module);

/// @brief Load multiple modules.
/// @param ctx The interpreter context.
/// @param modules The AST modules to load.
/// @return true on success.
/// @throws InterpreterError if loading fails.
bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules);

// ─── Hot-Reload ─────────────────────────────────────────────────────────

/// @brief Hot-reload a module.
/// @param ctx The interpreter context.
/// @param module The new AST module.
/// @param name The module name to replace.
/// @return true on success.
/// @throws InterpreterError if hot-reload fails.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, InternedString name);

/// @brief Hot-reload with string name.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name);

// ─── Foreign Libraries ─────────────────────────────────────────────────

/// @brief Register a foreign library.
/// @param ctx The interpreter context.
/// @param name The library name.
/// @throws InterpreterError if loading fails.
void registerLibrary(InterpreterContext& ctx, const std::string& name);

/// @brief Register libraries from a module.
void registerLibraries(InterpreterContext& ctx, ModuleAST* module);

/// @brief Register libraries from multiple modules.
void registerLibraries(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules);

// ─── Symbol Lookup ──────────────────────────────────────────────────────

/// @brief Look up a symbol in the JIT.
/// @param ctx The interpreter context.
/// @param name The symbol name.
/// @return Pointer to the symbol, or nullptr if not found.
void* lookupSymbol(InterpreterContext& ctx, const std::string& name);

/// @brief Look up a symbol by InternedString.
void* lookupSymbol(InterpreterContext& ctx, InternedString name);

// ─── Accessors ──────────────────────────────────────────────────────────

/// @brief Get the loaded modules.
std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx);

} // namespace interpreter