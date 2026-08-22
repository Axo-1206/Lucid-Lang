/// @file cli/run.cpp
/// @brief Implementation of the 'lucid run' command.

#include "run.hpp"
#include "../CLIContext.hpp"
#include "core/trace//Trace.hpp"
#include "../pipeline/Pipeline.hpp"

#include "interpreter/Interpreter.hpp"
#include "interpreter/support/InterpreterError.hpp"

#include <iostream>
#include <signal.h>
#include <atomic>
#include <chrono>

namespace cli {

static std::atomic<bool> g_running{true};

int runCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);
    
    bool verbose = opts.verbose || opts.interpreter.verbose;
    
    // ─── Run pipeline up to CodeGen ────────────────────────────────────
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::CodeGen;
    
    frontend::PipelineResult result = frontend::runPipeline(pipelineOpts, ctx);
    
    if (!result.success) {
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }
    
    // ─── Initialize Interpreter ────────────────────────────────────────
    Trace::info("Initializing interpreter");
    
    interpreter::InterpreterOptions interpOpts = opts.interpreter;
    interpOpts.verbose = verbose;
    interpOpts.entryPoint = opts.entryPoint;
    
    interpreter::InterpreterContext interpCtx(ctx.stringPool, ctx.diagnostics);
    interpreter::initialize(interpCtx, interpOpts);
    
    // ─── Execute ───────────────────────────────────────────────────────
    InternedString entryPoint = ctx.stringPool.intern(opts.entryPoint);
    
    Trace::info("Executing entry point: ", opts.entryPoint);
    
    interpreter::ExecutionResult execResult;
    try {
        auto start = std::chrono::steady_clock::now();
        execResult = interpreter::runModules(interpCtx, result.modules, entryPoint, false);
        auto end = std::chrono::steady_clock::now();
        execResult.executionTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    } catch (const interpreter::InterpreterError& e) {
        Trace::error("Interpreter error: ", e.what());
        return 1;
    } catch (const std::exception& e) {
        Trace::error("Unexpected error: ", e.what());
        return 1;
    }
    
    if (!execResult.success) {
        Trace::error("Execution failed: ", execResult.errorMessage);
        return execResult.exitCode;
    }
    
    // ─── Success Summary ───────────────────────────────────────────────
    std::cout << "\nExecution completed successfully!\n";
    std::cout << "   Exit code: " << execResult.exitCode << "\n";
    std::cout << "   Time:      " << execResult.executionTimeMs << " ms\n";
    std::cout << "   Modules:   " << result.modules.size() << "\n";
    
    if (ctx.diagnostics.hasWarnings()) {
        std::cout << "   Warnings:  " << ctx.diagnostics.warningCount() << "\n";
    }
    
    // ─── Hot-Reload (if enabled) ──────────────────────────────────────
    if (opts.enableHotReload) {
        Trace::info("Hot-reload enabled, starting file watcher");
        std::cout << "\nHot-reload active. Press Ctrl+C to exit.\n";
        
        // TODO: Implement hot-reload logic
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "\nShutting down...\n";
    }
    
    return 0;
}

} // namespace cli