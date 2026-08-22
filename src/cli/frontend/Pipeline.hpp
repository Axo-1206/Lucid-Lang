/// @file cli/frontend/Pipeline.hpp
/// @brief Compiler pipeline with configurable stop points and output.

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
    
    // Intermediate results (only valid if stop point reached)
    std::vector<ModuleAST*> modules;
    std::string errorMessage;
    
    // Code generation results
    std::string llvmIR;  // If stopAt == CodeGen
};

/**
 * @brief Run the compiler pipeline up to a specified stage.
 *
 * This is the single entry point for all frontend commands.
 * It runs the pipeline stages in order and stops at the specified stage.
 *
 * @param opts CLI options (contains rootFilePath and stopAt)
 * @param ctx CLI context (StringPool, ASTArena, diagnostics)
 * @return PipelineResult with the results of the pipeline
 *
 * ─── Pipeline Stages ──────────────────────────────────────────────────────
 *
 *   Lex     → Tokenize the source file
 *   Parse   → Build AST from tokens
 *   Sema    → Run semantic analysis on AST
 *   CodeGen → Generate LLVM IR from validated AST
 *   Execute → JIT compile and run (full execution)
 */
PipelineResult runPipeline(const CLIOptions& opts, CLIContext& ctx);

/**
 * @brief Run the compiler pipeline and output results.
 *
 * This handles output formatting (text or JSON) and file writing.
 *
 * @param opts CLI options
 * @param ctx CLI context
 * @return Exit code (0 for success, 1 for error)
 */
int runPipelineAndOutput(const CLIOptions& opts, CLIContext& ctx);

} // namespace frontend
} // namespace cli