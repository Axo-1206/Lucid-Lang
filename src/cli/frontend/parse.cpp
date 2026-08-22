/// @file cli/frontend/parse.cpp
/// @brief Implementation of 'lucid parse' command.

#include "parse.hpp"
#include "Pipeline.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"

namespace cli {
namespace frontend {

int parseCommand(const CLIOptions& opts) {
    // Override the stop point to Parse
    CLIOptions parseOpts = opts;
    parseOpts.stopAt = PipelineStage::Parse;
    
    // Initialize context
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);
    
    // Run the pipeline
    return runPipelineAndOutput(parseOpts, ctx);
}

} // namespace frontend
} // namespace cli