/// @file cli/frontend/Pipeline.cpp
/// @brief Implementation of the unified compiler pipeline.

#include "Pipeline.hpp"
#include "JSONDumper.hpp"
#include "core/trace/Trace.hpp"

#include "core/diagnostics/Diagnostic.hpp"
#include "parser/ModuleResolver.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

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

static bool directoryExists(const std::string& path) {
    return std::filesystem::exists(path) &&
           std::filesystem::is_directory(path);
}

static std::string getAbsolutePath(const std::string& path) {
    return std::filesystem::absolute(path).string();
}

static std::string getFileName(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

static std::string getStem(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

static bool isAbsolutePath(const std::string& path) {
    return std::filesystem::path(path).is_absolute();
}

static std::string resolvePath(const std::string& path) {
    if (isAbsolutePath(path)) {
        return path;
    }
    return std::filesystem::absolute(path).string();
}

/**
 * @brief Determine the output file path from user-specified output.
 * 
 * If output is a directory, use a default filename based on the input file.
 * If output is a file, use it as-is.
 * If output is not specified, return empty (use stdout).
 * 
 * @param opts CLI options
 * @return std::optional<std::string> The resolved output path, or empty for stdout
 */
static std::optional<std::string> resolveOutputPath(const CLIOptions& opts) {
    if (!opts.outputFile.has_value()) {
        return std::nullopt;
    }
    
    std::string outputPath = opts.outputFile.value();
    
    // ─── Check if it's an absolute path ──────────────────────────────
    std::string resolvedPath = resolvePath(outputPath);
    
    // ─── Check if it's a directory ────────────────────────────────────
    // If it ends with '/' or '\', it's definitely a directory
    if (outputPath.back() == '/' || outputPath.back() == '\\') {
        std::string stem = getStem(opts.rootFilePath);
        std::string ext = "json";
        std::string defaultName = stem + "." + ext;
        std::filesystem::path dirPath(resolvedPath);
        return (dirPath / defaultName).string();
    }
    
    // Check if it's an existing directory
    if (directoryExists(resolvedPath)) {
        std::string stem = getStem(opts.rootFilePath);
        std::string ext = "json";
        std::string defaultName = stem + "." + ext;
        std::filesystem::path dirPath(resolvedPath);
        return (dirPath / defaultName).string();
    }
    
    // ─── It's a file path ─────────────────────────────────────────────
    return resolvedPath;
}

/**
 * @brief Create parent directories for a file path if they don't exist.
 * 
 * @param filePath The file path to create directories for
 * @return bool True if successful or directories already exist
 */
static bool createParentDirectories(const std::string& filePath) {
    try {
        std::filesystem::path path(filePath);
        std::filesystem::path parent = path.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
            Trace::detail("Created directory: ", parent.string());
        }
        return true;
    } catch (const std::exception& e) {
        Trace::error("Failed to create directories for: ", filePath, " - ", e.what());
        return false;
    }
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

    /// IMPORTANT: We do NOT return early on parse errors!
    /// We want to keep the partial AST for debugging and analysis.
    /// The result.modules will contain whatever was successfully parsed,
    /// and ctx.diagnostics.hasErrors() tells us if there were errors.
    /// The JSON dumper will include both the partial AST and the diagnostics.

    if (ctx.diagnostics.hasErrors()) {
        Trace::error("Parsing encountered errors (partial AST available)");
        // We still have result.modules with partial AST
        // The diagnostics will be included in the JSON output
    }

    Trace::info("Parsed ", result.modules.size(), " module(s)");

    // If command is 'parse', stop here and return (even if there were errors)
    if (opts.stopAt == PipelineStage::Parse) {
        // Report errors BEFORE the "stopped" message
        if (ctx.diagnostics.hasErrors()) {
            std::cerr << "\n";
            ctx.diagnostics.dump(std::cerr);
            Trace::detail("Partial AST available despite parse errors");
        }
        Trace::detail("Stopped at Parse stage");
        return result;
    }

    // ─── 3. Semantic Analysis Stage ────────────────────────────────────
    // This stage runs for sema, run, and build commands
    // Only run semantic analysis if there were no parsing errors, or if
    // we want to attempt to recover partial semantic info.
    // Currently, we skip sema if there were parse errors to avoid crashes.
    if (!ctx.diagnostics.hasErrors()) {
        Trace::info("Running semantic analysis");
        
        sema::SemaContext semaCtx(ctx.stringPool, ctx.astArena, ctx.diagnostics);
        sema::analyze(result.modules, semaCtx);

        if (ctx.diagnostics.hasErrors()) {
            Trace::error("Semantic analysis encountered errors (partial AST available)");
        } else {
            Trace::info("Semantic analysis complete");
        }
    } else {
        Trace::detail("Skipping semantic analysis due to parse errors");
    }

    // If command is 'sema', stop here and return (even if there were errors)
    if (opts.stopAt == PipelineStage::Sema) {
        // Report errors BEFORE the "stopped" message
        if (ctx.diagnostics.hasErrors()) {
            std::cerr << "\n";
            ctx.diagnostics.dump(std::cerr);
            Trace::detail("Partial AST available despite semantic errors");
        }
        Trace::detail("Stopped at Sema stage");
        return result;
    }

    // ─── 4. Code Generation Stage ──────────────────────────────────────
    // This stage runs for run and build commands
    // Skip codegen if there were errors (can't generate valid IR from invalid AST)
    if (!ctx.diagnostics.hasErrors()) {
        if (opts.stopAt == PipelineStage::CodeGen || 
            opts.stopAt == PipelineStage::Execute ||
            opts.stopAt == PipelineStage::Build) {
            
            Trace::info("Generating code");
            
            // TODO: Implement code generation
            result.llvmIR = "; LLVM IR generation not yet implemented\n";
            Trace::info("Stopped at CodeGen stage (LLVM IR generation)");
        }
    } else {
        Trace::detail("Skipping code generation due to errors");
    }

    // ─── 5. Return ──────────────────────────────────────────────────────
    // The caller (run.cpp or build.cpp) handles execution/building
    return result;
}

// ─── Output Handler ─────────────────────────────────────────────────────

int writePipelineOutput(const CLIOptions& opts, 
                        const PipelineResult& result, 
                        CLIContext& ctx) {
    // Always write output regardless of errors (for debugging)
    // The JSON output will include the diagnostics so the user knows
    // what went wrong.

    // ─── JSON Output ──────────────────────────────────────────────────
    if (opts.outputFormat == OutputFormat::Json || 
        opts.outputFormat == OutputFormat::JsonPretty) {
        
        bool pretty = (opts.outputFormat == OutputFormat::JsonPretty);
        JSONDumper jsonDumper(ctx.stringPool, result.modules, pretty);
        
        std::string jsonOutput = jsonDumper.dump(ctx.diagnostics);
        
        // Resolve output path
        auto resolvedPath = resolveOutputPath(opts);
        
        if (resolvedPath.has_value()) {
            // Write to file (overwrite existing)
            std::string filePath = resolvedPath.value();
            
            // Create parent directories if needed
            if (!createParentDirectories(filePath)) {
                return 1;
            }
            
            // Write file (always overwrite - we expect fresh results)
            std::ofstream file(filePath, std::ios::trunc);
            if (!file.is_open()) {
                Trace::error("Failed to open file for writing: ", filePath);
                return 1;
            }
            
            file << jsonOutput;
            file.close();
            
            Trace::info("JSON output written to: ", filePath);
            std::cout << "\nOutput written to: " << filePath << "\n";
        } else {
            // Write to stdout
            std::cout << jsonOutput << "\n";
        }
    } else {
        // ─── Text Output ──────────────────────────────────────────────
        // Only show summary for parse/sema commands (not run)
        if (opts.command != CLIOptions::Command::Run) {
            std::cout << "\n[Pipeline] ";
            
            if (ctx.diagnostics.hasErrors()) {
                std::cout << "Completed with errors!\n";
                std::cout << "────────────────────────────────────────────────────\n";
                ctx.diagnostics.dump(std::cout);
                std::cout << "────────────────────────────────────────────────────\n";
                std::cout << "Partial AST available (use --json to see it)\n";
            } else {
                std::cout << "Success!\n";
            }
            
            std::cout << "  Modules:          " << result.modules.size() << "\n";
            
            if (ctx.diagnostics.hasErrors()) {
                std::cout << "  Errors:           " << ctx.diagnostics.errorCount() << "\n";
            }
            
            if (ctx.diagnostics.hasWarnings()) {
                std::cout << "  Warnings:         " << ctx.diagnostics.warningCount() << "\n";
            }
            
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
                default:
                    std::cout << "Unknown\n";
                    break;
            }
            
            std::cout << "\n[Pipeline] Tip: Use --json or --json-pretty to see the AST.\n";
        } else {
            // Run command - show errors if any
            if (ctx.diagnostics.hasErrors()) {
                std::cerr << "\n[Pipeline] Errors detected!\n";
                ctx.diagnostics.dump(std::cerr);
                return 1;
            }
        }
    }

    return 0;
}

} // namespace frontend
} // namespace cli