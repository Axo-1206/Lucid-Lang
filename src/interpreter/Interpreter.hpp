/// @file Interpreter.hpp
/// @brief Main interpreter API - procedural style.

#pragma once

#include "core/InterpreterContext.hpp"
#include "core/ModuleRegistry.hpp"
#include "support/InterpreterOptions.hpp"
#include "support/ExecutionResult.hpp"

#include <vector>

namespace interpreter {

// ─── Initialization ─────────────────────────────────────────────────────

void initialize(InterpreterContext& ctx, 
                const InterpreterOptions& options = InterpreterOptions{});

bool isInitialized(const InterpreterContext& ctx);

// ─── Execution ──────────────────────────────────────────────────────────

/// @brief Run modules.
/// @param ctx The interpreter context.
/// @param modules The AST modules to run (in dependency order).
/// @param entryPoint The entry point name (defaults to "main").
/// @param isHotReload Whether this is a hot-reload operation.
/// @return Execution result.
ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint = InternedString(),
                           bool isHotReload = false);

/// @brief Run a single module.
ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          InternedString entryPoint = InternedString(),
                          bool isHotReload = false);

// ─── Convenience Accessors ─────────────────────────────────────────────

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx);

} // namespace interpreter