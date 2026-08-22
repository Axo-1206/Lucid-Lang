/// @file cli/frontend/Pipeline.hpp
/// @brief Unified compiler pipeline with configurable stop points.

#pragma once

#include "cli/CLIOptions.hpp"
#include "cli/CLIContext.hpp"

#include "parser/Parser.hpp"
#include "sema/Sema.hpp"

#include <vector>
#include <string>

namespace cli {
namespace frontend {

/**
 * @brief Result of running the compiler pipeline.
 */
struct PipelineResult {
    bool success = false;
    int exitCode = 0;
    
    /// @brief Parsed modules (valid after Parse stage)
    std::vector<ModuleAST*> modules;
    
    /// @brief Error message if pipeline failed
    std::string errorMessage;
    
    /// @brief LLVM IR (valid after CodeGen stage)
    std::string llvmIR;
    
    /// @brief The stage where the pipeline stopped
    PipelineStage stoppedAt = PipelineStage::Lex;
};

/**
 * @brief Run the compiler pipeline up to a specified stage.
 *
 * This is the SINGLE entry point for all frontend commands.
 * 
 * ─── Pipeline Stages ──────────────────────────────────────────────────────
 *
 *   Parse   → Build AST from tokens (stopAt = Parse)
 *   Sema    → Run semantic analysis on AST (stopAt = Sema)
 *   CodeGen → Generate LLVM IR (stopAt = CodeGen)
 *   Execute → JIT compile and run (stopAt = Execute, handled by run.cpp)
 *   Build   → AOT compile to native binary (stopAt = Build, handled by build.cpp)
 *
 * ─── Command Mapping ──────────────────────────────────────────────────────
 *
 *   lucid parse  → stopAt = Parse,  output = AST (JSON or text)
 *   lucid sema   → stopAt = Sema,   output = AST + types (JSON or text)
 *   lucid run    → stopAt = Execute, output = execution result (no AST)
 *   lucid build  → stopAt = Build,  output = native binary
 *
 * ─── Output Behavior ──────────────────────────────────────────────────────
 *
 *   - JSON output: Always sent to stdout (or file with -o)
 *   - Text output: 
 *     - parse/sema: Show summary + optional AST (via --dump-ast)
 *     - run: Show execution summary with emojis
 *     - build: Show build summary
 *   - Trace output: Sent to stderr (controlled by --verbose/--trace)
 */
PipelineResult runPipeline(const CLIOptions& opts, CLIContext& ctx);

} // namespace frontend
} // namespace cli