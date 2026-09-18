/// @file cli/commands/sema.cpp
/// @brief Implementation of 'lucid sema' command.

#include "sema.hpp"
#include "../pipeline/Pipeline.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"
#include "core/trace/Trace.hpp"

namespace cli::commands {

int semaCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);

    // ─── Run pipeline up to Sema stage ──────────────────────────────
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::Sema;

    pipeline::PipelineResult result = pipeline::runPipeline(pipelineOpts, ctx);

    if (!result.success) {
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }

    // ─── Write output ─────────────────────────────────────────────────
    return pipeline::writePipelineOutput(opts, result, ctx);
}

} // namespace cli::commands