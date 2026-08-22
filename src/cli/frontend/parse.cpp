/// @file cli/frontend/parse.cpp
/// @brief Implementation of 'lucid parse' command.

#include "parse.hpp"
#include "Pipeline.hpp"
#include "JSONDumper.hpp"
#include "../CLIContext.hpp"
#include "../CLIOptions.hpp"
#include "cli/Trace.hpp"

namespace cli {
namespace frontend {

int parseCommand(const CLIOptions& opts) {
    // ─── Initialize context ────────────────────────────────────────────
    std::filesystem::path packageRoot = std::filesystem::current_path();
    CLIContext ctx(packageRoot);
    
    // ─── Run pipeline up to Parse stage ──────────────────────────────
    CLIOptions pipelineOpts = opts;
    pipelineOpts.stopAt = PipelineStage::Parse;
    
    PipelineResult result = runPipeline(pipelineOpts, ctx);
    
    if (!result.success) {
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }
    
    // ─── Output ────────────────────────────────────────────────────────
    if (opts.outputFormat == OutputFormat::Json || 
        opts.outputFormat == OutputFormat::JsonPretty) {
        bool pretty = (opts.outputFormat == OutputFormat::JsonPretty);
        JSONDumper jsonDumper(ctx.stringPool, result.modules, pretty);
        
        std::string jsonOutput = jsonDumper.dump(ctx.diagnostics);
        
        if (opts.outputFile.has_value()) {
            if (!jsonDumper.dumpToFile(ctx.diagnostics, opts.outputFile.value())) {
                Trace::error("Failed to write JSON output to: ", opts.outputFile.value());
                return 1;
            }
            Trace::info("JSON output written to: ", opts.outputFile.value());
        } else {
            std::cout << jsonOutput << "\n";
        }
    } else {
        // Text summary
        std::cout << "\n[Parse] Success!\n";
        std::cout << "  Modules: " << result.modules.size() << "\n";
        
        if (ctx.diagnostics.hasWarnings()) {
            std::cout << "  Warnings: " << ctx.diagnostics.warningCount() << "\n";
        }
        
        std::cout << "\n[Parse] Tip: Use --json or --json-pretty to see the AST.\n";
    }
    
    return 0;
}

} // namespace frontend
} // namespace cli