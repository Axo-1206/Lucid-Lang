/// @file cli/run.cpp
/// @brief Implementation of the 'lucid run' command.

#include "run.hpp"
#include "CLIContext.hpp"
#include "CLIOptions.hpp"
#include "frontend/Pipeline.hpp"

#include "interpreter/Interpreter.hpp"
#include "interpreter/support/InterpreterError.hpp"

#include <iostream>
#include <signal.h>
#include <atomic>

namespace cli {

static std::atomic<bool> g_running{true};

int runCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);
    
    bool verbose = opts.verbose || opts.interpreter.verbose;
    
    // ─── Run the pipeline up to CodeGen ───────────────────────────────
    // The pipeline handles parsing, semantic analysis, and code generation
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::CodeGen;
    
    frontend::PipelineResult result = frontend::runPipeline(pipelineOpts, ctx);
    
    if (!result.success) {
        std::cerr << "\n❌ Pipeline failed: " << result.errorMessage << "\n";
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }
    
    if (verbose) {
        std::cout << "✅ Pipeline complete, ready for execution" << std::endl;
    }
    
    // ─── Initialize Interpreter ────────────────────────────────────────
    interpreter::InterpreterOptions interpOpts = opts.interpreter;
    interpOpts.verbose = verbose;
    interpOpts.entryPoint = opts.entryPoint;
    
    interpreter::InterpreterContext interpCtx(ctx.stringPool, ctx.diagnostics);
    interpreter::initialize(interpCtx, interpOpts);
    
    // ─── Execute ───────────────────────────────────────────────────────
    InternedString entryPoint = ctx.stringPool.intern(opts.entryPoint);
    
    if (verbose) {
        std::cout << "⏳ Executing..." << std::endl;
    }
    
    interpreter::ExecutionResult execResult;
    try {
        execResult = interpreter::runModules(interpCtx, result.modules, entryPoint, false);
    } catch (const interpreter::InterpreterError& e) {
        std::cerr << "❌ Interpreter error: " << e.what() << std::endl;
        return 1;
    }
    
    if (!execResult.success) {
        std::cerr << "❌ Execution failed: " << execResult.errorMessage << std::endl;
        return execResult.exitCode;
    }
    
    if (verbose) {
        std::cout << "✅ Execution completed in " << execResult.executionTimeMs << "ms" << std::endl;
        std::cout << "   Exit code: " << execResult.exitCode << std::endl;
    }
    
    // ─── Hot-Reload (if enabled) ──────────────────────────────────────
    // ... (hot-reload code from your original run.cpp)
    
    return 0;
}

} // namespace cli