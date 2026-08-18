/// @file cli/run.cpp
/// @brief Implementation of the 'lucid run' command.

#include "CLIOptions.hpp"
#include "CLIContext.hpp"
#include "DependencyGraph.hpp"
#include "FileWatcher.hpp"
#include "run.hpp"

#include "parser/Parser.hpp"
#include "sema/Sema.hpp"
#include "interpreter/Interpreter.hpp"
#include "interpreter/support/InterpreterError.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <signal.h>
#include <atomic>
#include <thread>

namespace cli {

// ─── Global state for signal handling ──────────────────────────────────

static std::atomic<bool> g_running{true};

void signalHandler(int signum) {
    (void)signum;
    g_running = false;
}

// ─── File I/O Helper ──────────────────────────────────────────────────

/**
 * @brief Read a file's contents into a string.
 *
 * @note This is the ONLY place we do file I/O for reading source files.
 *       ModuleResolver uses this via its own methods, but the CLI still
 *       needs to read the initial root file directly.
 *
 * @param path The file path to read.
 * @return std::string The file contents (empty string on error, but empty
 *         files are also valid — check the error state separately).
 */
static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static bool fileExists(const std::string& path) {
    return std::filesystem::exists(path) &&
           std::filesystem::is_regular_file(path);
}

static std::string getAbsolutePath(const std::string& path) {
    return std::filesystem::absolute(path).string();
}

// ─── Hot‑Reload Handler ─────────────────────────────────────────────────

static void handleFileChange(
    CLIContext& ctx,
    const DependencyGraph& depGraph,
    interpreter::InterpreterContext& interpCtx,
    const std::string& changedRelativePath,
    const CLIOptions& opts
) {
    InternedString changedName = ctx.stringPool.intern(changedRelativePath);

    std::cout << "[Hot‑reload] File changed: " << changedRelativePath << std::endl;

    // 1. Get affected modules (changed + all dependents)
    std::vector<InternedString> affectedNames = depGraph.getAffected(changedName);

    // 2. Re-parse affected modules
    std::vector<ModuleAST*> affectedModules;
    affectedModules.reserve(affectedNames.size());

    for (InternedString name : affectedNames) {
        std::string filePath = ctx.stringPool.lookup(name);

        // Use ModuleResolver to get the full path and read the source
        parser::ModuleResolver resolver(ctx.packageRoot, ctx.stringPool);
        
        std::filesystem::path fullPath = resolver.getModuleFilePath(name);
        
        // Read source using ModuleResolver
        std::string source = resolver.readModuleSource(name);
        
        // Note: source.empty() is valid for empty files!
        // ModuleResolver::readModuleSource returns empty string for both
        // empty files AND missing files. We need to check file existence.
        if (!resolver.moduleFileExists(name)) {
            std::cerr << "[Hot‑reload] Error: File not found: " << fullPath.string() << std::endl;
            return;
        }

        parser::ParserContext parserCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics, &resolver);
        ModuleAST* module = parser::parse(filePath, source, parserCtx);

        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
            return;
        }
        affectedModules.push_back(module);
    }

    // 3. Re-run Sema
    sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);
    sema::analyze(affectedModules, semaCtx);

    if (ctx.diagnostics.hasErrors()) {
        ctx.diagnostics.dump(std::cerr);
        return;
    }

    // 4. Hot reload via interpreter
    try {
        InternedString entryPoint = ctx.stringPool.intern(opts.entryPoint);

        interpreter::ExecutionResult result = interpreter::runModules(
            interpCtx,
            affectedModules,
            entryPoint,
            true  // isHotReload
        );

        if (result.success) {
            std::cout << "[Hot‑reload] ✅ Successfully reloaded "
                      << affectedModules.size() << " module(s)" << std::endl;
        } else {
            std::cerr << "[Hot‑reload] ❌ Failed: " << result.errorMessage << std::endl;
        }
    } catch (const interpreter::InterpreterError& e) {
        std::cerr << "[Hot‑reload] ❌ Interpreter error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Hot‑reload] ❌ Unexpected error: " << e.what() << std::endl;
    }
}

// ─── Main Run Command ──────────────────────────────────────────────────

int runCommand(const CLIOptions& opts) {
    // ─── 1. Validate input ─────────────────────────────────────────────
    std::string absRootPath = getAbsolutePath(opts.rootFilePath);
    if (!fileExists(absRootPath)) {
        std::cerr << "Error: File not found: " << absRootPath << std::endl;
        return 1;
    }

    std::string source = readFile(absRootPath);

    // ─── 2. Initialize CLI context ─────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);

    bool verbose = opts.verbose || opts.interpreter.verbose;

    if (verbose) {
        std::cout << "[CLI] Package root: " << packageRoot.string() << std::endl;
        std::cout << "[CLI] Root file: " << absRootPath << std::endl;
    }

    // ─── 3. Parse ───────────────────────────────────────────────────────
    parser::ModuleResolver resolver(packageRoot, ctx.stringPool);
    parser::ParserContext parserCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics, &resolver);

    if (verbose) {
        std::cout << "[Parser] Parsing program..." << std::endl;
    }

    std::vector<ModuleAST*> modules = parser::parseProgram(absRootPath, source, parserCtx);

    if (ctx.diagnostics.hasErrors()) {
        ctx.diagnostics.dump(std::cerr);
        return 1;
    }

    if (verbose) {
        std::cout << "[Parser] Parsed " << modules.size() << " module(s)" << std::endl;
    }

    // ─── 4. Semantic Analysis ──────────────────────────────────────────
    sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);

    if (verbose) {
        std::cout << "[Sema] Analyzing..." << std::endl;
    }

    sema::analyze(modules, semaCtx);

    if (ctx.diagnostics.hasErrors()) {
        ctx.diagnostics.dump(std::cerr);
        return 1;
    }

    if (verbose) {
        std::cout << "[Sema] Analysis complete" << std::endl;
    }

    // ─── 5. Build Dependency Graph ─────────────────────────────────────
    DependencyGraph depGraph;
    depGraph.build(modules);

    if (verbose) {
        std::cout << "[CLI] Dependency graph: " << depGraph.size() << " node(s)" << std::endl;
        for (InternedString name : depGraph.getAllModules()) {
            auto imports = depGraph.getImports(name);
            if (!imports.empty()) {
                std::cout << "  " << ctx.stringPool.lookup(name) << " → [";
                for (size_t i = 0; i < imports.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << ctx.stringPool.lookup(imports[i]);
                }
                std::cout << "]" << std::endl;
            }
        }
    }

    // ─── 6. Initialize Interpreter ─────────────────────────────────────
    // Build interpreter options: start with CLI's nested options,
    // then override with CLI-specific fields if they were explicitly set.
    interpreter::InterpreterOptions interpOpts = opts.interpreter;
    interpOpts.verbose = verbose;
    interpOpts.entryPoint = opts.entryPoint;

    interpreter::InterpreterContext interpCtx(ctx.stringPool, ctx.diagnostics);
    interpreter::initialize(interpCtx, interpOpts);

    if (verbose) {
        std::cout << "[Interpreter] Initialized" << std::endl;
    }

    // ─── 7. Initial Execution ──────────────────────────────────────────
    InternedString entryPoint = ctx.stringPool.intern(opts.entryPoint);

    if (verbose) {
        std::cout << "[Interpreter] Running initial execution..." << std::endl;
    }

    interpreter::ExecutionResult result;
    try {
        result = interpreter::runModules(interpCtx, modules, entryPoint, false);
    } catch (const interpreter::InterpreterError& e) {
        std::cerr << "[Interpreter] Error: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[Interpreter] Unexpected error: " << e.what() << std::endl;
        return 1;
    }

    if (!result.success) {
        std::cerr << "[Interpreter] Execution failed: " << result.errorMessage << std::endl;
        return result.exitCode;
    }

    if (verbose) {
        std::cout << "[Interpreter] Execution completed in "
                  << result.executionTimeMs << "ms" << std::endl;
        std::cout << "[Interpreter] Exit code: " << result.exitCode << std::endl;
    }

    // ─── 8. Hot‑Reload (if enabled) ────────────────────────────────────
    if (opts.enableHotReload) {
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);

        std::vector<std::string> watchedFiles;
        watchedFiles.reserve(depGraph.size());

        parser::ModuleResolver resolver(packageRoot, ctx.stringPool);

        for (InternedString name : depGraph.getAllModules()) {
            std::filesystem::path absPath = resolver.getModuleFilePath(name);
            watchedFiles.push_back(absPath.string());
        }

        FileWatcher watcher(packageRoot, [&](const std::string& changedPath) {
            handleFileChange(ctx, depGraph, interpCtx, changedPath, opts);
        });

        watcher.watchFiles(watchedFiles);
        watcher.start();

        std::cout << "[Hot‑reload] Watching " << watchedFiles.size()
                  << " file(s). Press Ctrl+C to exit." << std::endl;

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        watcher.stop();
        std::cout << "\n[Hot‑reload] Shutting down..." << std::endl;
    }

    return 0;
}

} // namespace cli