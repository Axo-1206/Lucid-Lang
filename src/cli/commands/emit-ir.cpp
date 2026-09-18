/// @file cli/commands/emit-ir.cpp
/// @brief Implementation of 'lucid emit-ir' command.

#include "emit-ir.hpp"
#include "../pipeline/Pipeline.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"
#include "core/trace/Trace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace cli::commands {

int emitIRCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);

    // ─── Run pipeline up to EmitIR ─────────────────────────────────────
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::EmitIR;

    pipeline::PipelineResult result = pipeline::runPipeline(pipelineOpts, ctx);

    if (!result.success) {
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }

    // ─── Write output ──────────────────────────────────────────────────
    if (opts.outputFile.has_value()) {
        std::ofstream out(*opts.outputFile, std::ios::trunc);
        if (!out.is_open()) {
            Trace::error("Failed to open output file: ", *opts.outputFile);
            return 1;
        }
        out << result.llvmIR;
        Trace::info("IR written to: ", *opts.outputFile);
    } else {
        std::cout << result.llvmIR;
    }

    return 0;
}

} // namespace cli::commands