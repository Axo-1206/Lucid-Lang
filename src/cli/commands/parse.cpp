/// @file cli/commands/parse.cpp
/// @brief Implementation of 'lucid parse' command.

#include "parse.hpp"
#include "../pipeline/Pipeline.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"
#include "core/trace/Trace.hpp"

namespace cli::commands {

int parseCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);

    // ─── Run pipeline up to Parse stage ──────────────────────────────
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::Parse;

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