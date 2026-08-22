/// @file cli/frontend/Pipeline.cpp
/// @brief Implementation of the unified compiler pipeline.

#include "Pipeline.hpp"
#include "JSONDumper.hpp"
#include "cli/Trace.hpp"

#include "core/diagnostics/Diagnostic.hpp"
#include "parser/ModuleResolver.hpp"

#include <fstream>
#include <sstream>
#include <iostream>

namespace cli {
namespace frontend {

// ─── File I/O Helpers ──────────────────────────────────────────────────

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static bool fileExists(const std::string& path) {
    return std::filesystem::exists(path) &&
           std::filesystem::is_regular_file(path);
}

static std::string getAbsolutePath(const std::string& path) {
    return std::filesystem::absolute(path).string();
}

// ─── Pipeline Implementation ────────────────────────────────────────────

PipelineResult runPipeline(const CLIOptions& opts, CLIContext& ctx) {
    PipelineResult result;
    result.success = true;
    result.stoppedAt = opts.stopAt;
    
    // ─── 1. Validate input ─────────────────────────────────────────────
    std::string absRootPath = getAbsolutePath(opts.rootFilePath);
    if (!fileExists(absRootPath)) {
        result.success = false;
        result.exitCode = 1;
        result.errorMessage = "File not found: " + absRootPath;
        Trace::error("File not found: ", absRootPath);
        return result;
    }

    std::string source = readFile(absRootPath);
    
    // ─── 2. Parse Stage ────────────────────────────────────────────────
    // This stage always runs for all commands (parse, sema, run, build)
    Trace::info("Parsing: ", absRootPath);

    parser::ModuleResolver resolver(ctx.packageRoot, ctx.stringPool);
    parser::ParserContext parserCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics, &resolver);

    result.modules = parser::parseProgram(absRootPath, source, parserCtx);

    if (ctx.diagnostics.hasErrors()) {
        result.success = false;
        result.exitCode = 1;
        result.errorMessage = "Parsing failed";
        Trace::error("Parsing failed");
        return result;
    }

    Trace::info("Parsed ", result.modules.size(), " module(s)");

    // If command is 'parse', stop here and return
    if (opts.stopAt == PipelineStage::Parse) {
        Trace::detail("Stopped at Parse stage");
        return result;
    }

    // ─── 3. Semantic Analysis Stage ────────────────────────────────────
    // This stage runs for sema, run, and build commands
    Trace::info("Running semantic analysis");
    
    sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);
    sema::analyze(result.modules, semaCtx);

    if (ctx.diagnostics.hasErrors()) {
        result.success = false;
        result.exitCode = 1;
        result.errorMessage = "Semantic analysis failed";
        Trace::error("Semantic analysis failed");
        return result;
    }

    Trace::info("Semantic analysis complete");

    // If command is 'sema', stop here and return
    if (opts.stopAt == PipelineStage::Sema) {
        Trace::detail("Stopped at Sema stage");
        return result;
    }

    // ─── 4. Code Generation Stage ──────────────────────────────────────
    // This stage runs for run and build commands
    if (opts.stopAt == PipelineStage::CodeGen || 
        opts.stopAt == PipelineStage::Execute ||
        opts.stopAt == PipelineStage::Build) {
        
        Trace::info("Generating code");
        
        // TODO: Implement code generation
        result.llvmIR = "; LLVM IR generation not yet implemented\n";
        Trace::info("Stopped at CodeGen stage (LLVM IR generation)");
    }

    // ─── 5. Return ──────────────────────────────────────────────────────
    // The caller (run.cpp or build.cpp) handles execution/building
    return result;
}

} // namespace frontend
} // namespace cli