/// @file cli/run.cpp
/// @brief Implementation of the 'lucid run' command.

#include "CLIContext.hpp"
#include "DependencyGraph.hpp"
#include "FileWatcher.hpp"
#include "RunOptions.hpp"

#include "parser/Parser.hpp"
#include "sema/Sema.hpp"
#include "interpreter/Interpreter.hpp"
#include "interpreter/support/InterpreterError.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <signal.h>
#include <atomic>

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
    const RunOptions& options
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
        
        // Get absolute filesystem path
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

        // Parse the file
        parser::ParserContext parserCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics, &resolver);

        ModuleAST* module = parser::parse(filePath, source, parserCtx);
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
            return;
        }
        affectedModules.push_back(module);
    }

    // 3. Re-run Sema on affected modules
    sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);
    sema::analyze(affectedModules, semaCtx);

    if (ctx.diagnostics.hasErrors()) {
        ctx.diagnostics.dump(std::cerr);
        return;
    }

    // 4. Hot reload via interpreter
    try {
        InternedString entryPoint = ctx.stringPool.intern(options.entryPoint);

        interpreter::ExecutionResult result = interpreter::runModules(
            interpCtx,
            affectedModules,
            entryPoint,
            true  // isHotReload
        );

        if (result.success) {
            std::cout << "[Hot‑reload] Successfully reloaded "
                      << affectedModules.size()
                      << " module(s)" << std::endl;
        } else {
            std::cerr << "[Hot‑reload] Failed: " << result.errorMessage << std::endl;
        }
    } catch (const interpreter::InterpreterError& e) {
        std::cerr << "[Hot‑reload] Interpreter error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[Hot‑reload] Unexpected error: " << e.what() << std::endl;
    }
}

// ─── Main Run Command ──────────────────────────────────────────────────

int runCommand(const std::string& rootPath, const RunOptions& options) {
    // ─── 1. Validate input ─────────────────────────────────────────────
    std::string absRootPath = getAbsolutePath(rootPath);
    if (!fileExists(absRootPath)) {
        std::cerr << "Error: File not found: " << absRootPath << std::endl;
        return 1;
    }

    std::string source = readFile(absRootPath);
    // source.empty() is valid for empty files — we already checked fileExists

    // ─── 2. Initialize CLI context ─────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);

    if (options.verbose) {
        std::cout << "[CLI] Package root: " << packageRoot.string() << std::endl;
        std::cout << "[CLI] Root file: " << absRootPath << std::endl;
    }

    // ─── 3. Parse ───────────────────────────────────────────────────────
    // Note: The root module's file path is the relative path from package root
    std::string rootRelativePath = std::filesystem::relative(absRootPath, packageRoot).string();

    parser::ModuleResolver resolver(packageRoot, ctx.stringPool);
    parser::ParserContext parserCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics, &resolver);

    if (options.verbose) {
        std::cout << "[Parser] Parsing program..." << std::endl;
    }

    std::vector<ModuleAST*> modules = parser::parseProgram(absRootPath, source, parserCtx);

    if (ctx.diagnostics.hasErrors()) {
        ctx.diagnostics.dump(std::cerr);
        return 1;
    }

    if (options.verbose) {
        std::cout << "[Parser] Parsed " << modules.size() << " module(s)" << std::endl;
    }

    // ─── 4. Semantic Analysis ──────────────────────────────────────────
    sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);

    if (options.verbose) {
        std::cout << "[Sema] Analyzing..." << std::endl;
    }

    sema::analyze(modules, semaCtx);

    if (ctx.diagnostics.hasErrors()) {
        ctx.diagnostics.dump(std::cerr);
        return 1;
    }

    if (options.verbose) {
        std::cout << "[Sema] Analysis complete" << std::endl;
    }

    // ─── 5. Build Dependency Graph ─────────────────────────────────────
    DependencyGraph depGraph;
    depGraph.build(modules);

    if (options.verbose) {
        std::cout << "[CLI] Dependency graph: " << depGraph.size() << " node(s)" << std::endl;
        
        // Log dependencies for debugging
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
    interpreter::InterpreterOptions interpOpts;
    interpOpts.verbose = options.verbose;
    interpOpts.enableHotReload = options.enableHotReload;
    interpOpts.optimizationLevel = options.optimizationLevel;
    interpOpts.enableDebugInfo = options.enableDebugInfo;

    interpreter::InterpreterContext interpCtx(ctx.stringPool, ctx.diagnostics);
    interpreter::initialize(interpCtx, interpOpts);

    if (options.verbose) {
        std::cout << "[Interpreter] Initialized" << std::endl;
    }

    // ─── 7. Initial Execution ──────────────────────────────────────────
    InternedString entryPoint = ctx.stringPool.intern(options.entryPoint);

    if (options.verbose) {
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

    if (options.verbose) {
        std::cout << "[Interpreter] Execution completed in "
                  << result.executionTimeMs << "ms" << std::endl;
        std::cout << "[Interpreter] Exit code: " << result.exitCode << std::endl;
    }

    // ─── 8. Hot‑Reload (if enabled) ────────────────────────────────────
    if (options.enableHotReload) {
        // Set up signal handling for clean shutdown
        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);

        // Build list of all watched files (absolute paths)
        std::vector<std::string> watchedFiles;
        watchedFiles.reserve(depGraph.size());

        // Use ModuleResolver to get absolute paths
        parser::ModuleResolver resolver(packageRoot, ctx.stringPool);

        for (InternedString name : depGraph.getAllModules()) {
            std::filesystem::path absPath = resolver.getModuleFilePath(name);
            watchedFiles.push_back(absPath.string());
        }

        // Create file watcher
        FileWatcher watcher(packageRoot, [&](const std::string& changedPath) {
            // This is called from the watcher thread
            // changedPath is already relative to package root
            handleFileChange(ctx, depGraph, interpCtx, changedPath, options);
        });

        // Start watching
        watcher.watchFiles(watchedFiles);
        watcher.start();

        std::cout << "[Hot‑reload] Watching " << watchedFiles.size()
                  << " file(s). Press Ctrl+C to exit." << std::endl;

        // Wait for interrupt
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Clean shutdown
        watcher.stop();
        std::cout << "\n[Hot‑reload] Shutting down..." << std::endl;
    }

    return 0;
}

} // namespace cli