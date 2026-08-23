/// @file cli/frontend/sema.cpp
/// @brief Implementation of 'lucid sema' command.

#include "sema.hpp"
#include "../pipeline/Pipeline.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"
#include "core/trace/Trace.hpp"

namespace cli {
namespace frontend {

int semaCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);
    
    // ─── Run pipeline up to Sema stage ──────────────────────────────
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::Sema;
    
    PipelineResult result = runPipeline(pipelineOpts, ctx);
    
    if (!result.success) {
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }
    
    // ─── Write output ─────────────────────────────────────────────────
    return writePipelineOutput(opts, result, ctx);
}

} // namespace frontend
} // namespace cli