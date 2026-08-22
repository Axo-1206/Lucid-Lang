/// @file cli/frontend/sema.cpp
/// @brief Implementation of 'lucid sema' command.

#include "sema.hpp"
#include "Pipeline.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"

namespace cli {
namespace frontend {

int semaCommand(const CLIOptions& opts) {
    // Override the stop point to Sema
    CLIOptions semaOpts = opts;
    semaOpts.stopAt = PipelineStage::Sema;
    
    // Initialize context
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);
    
    // Run the pipeline
    return runPipelineAndDump(semaOpts, ctx);
}

} // namespace frontend
} // namespace cli