/// @file cli/run.hpp
/// @brief 'lucid run' command implementation.

#pragma once

#include "RunOptions.hpp"
#include "DependencyGraph.hpp"
#include "FileWatcher.hpp"
#include "interpreter/Interpreter.hpp"

#include <string>
#include <vector>

namespace cli {

// ─── Forward Declarations ──────────────────────────────────────────────

struct CLIContext;

// ─── Main Entry Point ──────────────────────────────────────────────────

/**
 * @brief Execute the 'lucid run' command.
 *
 * JIT interprets and executes a `.luc` file using LLVM's ORC JIT.
 *
 * ─── Detailed Flow ────────────────────────────────────────────────────
 *
 *  1. **Validation**: Check that the root file exists and is readable.
 *
 *  2. **Parsing**: Parse the entire program using `parser::parseProgram()`.
 *     This recursively resolves and parses all imported modules.
 *     `ModuleAST::imports` is populated with resolved file paths.
 *
 *  3. **Semantic Analysis**: Run `sema::analyze()` to validate types,
 *     resolve names, and annotate the AST.
 *
 *  4. **Dependency Graph**: Build a bi‑directional dependency graph
 *     from `ModuleAST::imports` for use by the file watcher.
 *
 *  5. **Interpreter Initialization**: Initialize the LLVM ORC JIT
 *     and set up the interpreter context.
 *
 *  6. **Initial Execution**: Run the program's entry point (default: `main`).
 *
 *  7. **Hot‑Reload** (if enabled): Start a file watcher thread that
 *     monitors all `.luc` files. On any change:
 *       - Re‑parse the changed file and all transitive dependents
 *       - Re‑run semantic analysis on affected modules
 *       - Hot‑reload via `interpreter::runModules(isHotReload=true)`
 *
 * ─── Example ───────────────────────────────────────────────────────────
 *
 * ```cpp
 * // Command line: lucid run main.luc --verbose --no-hot-reload
 *
 * cli::RunOptions opts;
 * opts.verbose = true;
 * opts.enableHotReload = false;
 * int exitCode = cli::runCommand("main.luc", opts);
 * ```
 *
 * @param rootPath Path to the root/main `.luc` file (relative or absolute).
 * @param options Run command options.
 * @return Exit code (0 = success, non‑zero = failure).
 */
int runCommand(const std::string& rootPath, const RunOptions& options);

// ─── Hot‑Reload Handler ────────────────────────────────────────────────

/**
 * @brief Handle a file change event during hot‑reload.
 *
 * This is called by the FileWatcher when a `.luc` file changes.
 *
 * @param ctx CLI context (StringPool, ASTArena, diagnostics)
 * @param depGraph Dependency graph (for finding affected modules)
 * @param interpCtx Interpreter context (for hot‑reload)
 * @param changedRelativePath Path to changed file (relative to package root)
 * @param options Run command options
 *
 * @note This runs on the file watcher thread, not the main thread.
 */
void handleHotReloadChange(
    CLIContext& ctx,
    const DependencyGraph& depGraph,
    interpreter::InterpreterContext& interpCtx,
    const std::string& changedRelativePath,
    const RunOptions& options
);

// ─── Signal Handling ────────────────────────────────────────────────────

/**
 * @brief Global flag indicating the program is running.
 *
 * Used by the main thread to wait for Ctrl+C / SIGTERM.
 */
extern std::atomic<bool> g_running;

/**
 * @brief Signal handler for graceful shutdown.
 *
 * Sets `g_running = false` to break the main event loop.
 *
 * @param signum Signal number (SIGINT, SIGTERM, etc.)
 */
void signalHandler(int signum);

} // namespace cli