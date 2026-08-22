/// @file cli/frontend/Pipeline.cpp
/// @brief Implementation of the compiler pipeline.

#include "Pipeline.hpp"
#include "JSONDumper.hpp"

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
    
    bool verbose = opts.verbose || opts.interpreter.verbose;
    
    // ─── 1. Validate input ─────────────────────────────────────────────
    std::string absRootPath = getAbsolutePath(opts.rootFilePath);
    if (!fileExists(absRootPath)) {
        result.success = false;
        result.exitCode = 1;
        result.errorMessage = "File not found: " + absRootPath;
        return result;
    }

    std::string source = readFile(absRootPath);
    
    // ─── 2. Parse ───────────────────────────────────────────────────────
    parser::ModuleResolver resolver(ctx.packageRoot, ctx.stringPool);
    parser::ParserContext parserCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics, &resolver);

    if (verbose) {
        std::cout << "[Pipeline] Parsing..." << std::endl;
    }

    result.modules = parser::parseProgram(absRootPath, source, parserCtx);

    if (ctx.diagnostics.hasErrors()) {
        result.success = false;
        result.exitCode = 1;
        result.errorMessage = "Parsing failed";
        return result;
    }

    if (verbose) {
        std::cout << "[Pipeline] Parsed " << result.modules.size() << " module(s)" << std::endl;
    }

    // ─── 3. Stop at Parse? ─────────────────────────────────────────────
    if (opts.stopAt == PipelineStage::Parse) {
        return result;
    }

    // ─── 4. Semantic Analysis ──────────────────────────────────────────
    sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);
    sema::analyze(result.modules, semaCtx);

    if (ctx.diagnostics.hasErrors()) {
        result.success = false;
        result.exitCode = 1;
        result.errorMessage = "Semantic analysis failed";
        return result;
    }

    if (verbose) {
        std::cout << "[Pipeline] Semantic analysis complete" << std::endl;
    }

    // ─── 5. Stop at Sema? ──────────────────────────────────────────────
    if (opts.stopAt == PipelineStage::Sema) {
        return result;
    }

    // ─── 6. Code Generation ─────────────────────────────────────────────
    if (opts.stopAt == PipelineStage::CodeGen) {
        // TODO: Implement code generation
        result.llvmIR = "; LLVM IR generation not yet implemented\n";
        if (verbose) {
            std::cout << "[Pipeline] Stopped at CodeGen (LLVM IR generation)" << std::endl;
        }
        return result;
    }

    // ─── 7. Full Execution ──────────────────────────────────────────────
    // The run command handles execution separately
    if (opts.command == CLIOptions::Command::Run) {
        if (verbose) {
            std::cout << "[Pipeline] Pipeline complete, handing off to interpreter" << std::endl;
        }
        return result;
    }

    return result;
}

// ─── Pipeline with Output ──────────────────────────────────────────────

int runPipelineAndOutput(const CLIOptions& opts, CLIContext& ctx) {
    bool verbose = opts.verbose || opts.interpreter.verbose;
    
    // ─── Run the pipeline ──────────────────────────────────────────────
    PipelineResult result = runPipeline(opts, ctx);
    
    if (!result.success) {
        std::cerr << "\n[Pipeline] Error: " << result.errorMessage << "\n";
        if (ctx.diagnostics.hasErrors()) {
            ctx.diagnostics.dump(std::cerr);
        }
        return result.exitCode;
    }

    // ─── Generate Output ──────────────────────────────────────────────
    
    if (opts.outputFormat == OutputFormat::Json || 
        opts.outputFormat == OutputFormat::JsonPretty) {
        // JSON output
        bool pretty = (opts.outputFormat == OutputFormat::JsonPretty);
        JSONDumper jsonDumper(ctx.stringPool, result.modules, pretty);
        
        std::string jsonOutput = jsonDumper.dump(ctx.diagnostics);
        
        if (opts.outputFile.has_value()) {
            // Write to file
            if (!jsonDumper.dumpToFile(ctx.diagnostics, opts.outputFile.value())) {
                std::cerr << "[Pipeline] Failed to write JSON output to: " 
                          << opts.outputFile.value() << "\n";
                return 1;
            }
            if (verbose) {
                std::cout << "[Pipeline] JSON output written to: " 
                          << opts.outputFile.value() << "\n";
            }
        } else {
            // Write to stdout
            std::cout << jsonOutput << "\n";
        }
    } else {
        // Text output (summary only)
        // Note: AST dumping is now done via JSON output. 
        // Use --json or --json-pretty for AST inspection.

        // ─── Summary ──────────────────────────────────────────────────
        std::cout << "\n[Pipeline] Success!\n";
        std::cout << "  Modules:          " << result.modules.size() << "\n";
        std::cout << "  Stopped at:       ";
        
        switch (opts.stopAt) {
            case PipelineStage::Lex:    
                std::cout << "Lex (tokenization)\n"; 
                break;
            case PipelineStage::Parse:  
                std::cout << "Parse (AST)\n"; 
                break;
            case PipelineStage::Sema:   
                std::cout << "Sema (semantic analysis)\n"; 
                break;
            case PipelineStage::CodeGen: 
                std::cout << "CodeGen (LLVM IR)\n"; 
                break;
            case PipelineStage::Execute: 
                std::cout << "Execute (full run)\n"; 
                break;
        }

        if (ctx.diagnostics.hasWarnings()) {
            std::cout << "  Warnings:         " << ctx.diagnostics.warningCount() << "\n";
        }
        
        std::cout << "\n[Pipeline] Tip: Use --json or --json-pretty to see the AST.\n";
    }

    return 0;
}

} // namespace frontend
} // namespace cli