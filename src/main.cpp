/// @file main.cpp
/// @brief Entry point for the Lucid Compiler CLI.
///
/// This file implements the command-line interface for the Lucid compiler,
/// providing multiple sub-commands that allow stopping at different stages
/// of the compilation pipeline.
///
/// ─── Available Commands ──────────────────────────────────────────────────────
///
/// | Command | Description                              | Stop Point      |
/// |---------|------------------------------------------|-----------------|
/// | `run`   | JIT compile and execute                  | Execute         |
/// | `parse` | Parse only (AST construction)            | Parse           |
/// | `sema`  | Parse + semantic analysis                | Sema            |
/// | `build` | AOT compile to native binary (future)    | CodeGen + Link  |
/// | `repl`  | Interactive REPL (future)                | N/A             |
///
/// ─── Pipeline Stages ──────────────────────────────────────────────────────────
///
/// Each command stops at a specific pipeline stage:
///
///   Lexer → Parser → Semantic Analysis → CodeGen → Execution
///            ↑           ↑                ↑          ↑
///         --parse    --sema           --codegen   --run (default)
///
/// ─── Command Examples ─────────────────────────────────────────────────────────
///
/// ## Parse Command (Stop After AST)
///   lucid parse main.luc                  # Parse with default output
///   lucid parse main.luc --json-pretty    # Parse and dump pretty JSON
///   lucid parse main.luc --verbose        # Parse with verbose output
///
/// ## Sema Command (Stop After Semantic Analysis)
///   lucid sema main.luc                   # Parse + semantic analysis
///   lucid sema main.luc --json            # Dump validated AST as JSON
///   lucid sema main.luc --verbose         # Show semantic analysis progress
///
/// ## Run Command (Full Execution)
///   lucid run main.luc                    # Execute main.luc
///   lucid run main.luc --verbose          # Show detailed execution info
///   lucid run main.luc --no-hot-reload    # Disable file watcher
///   lucid run main.luc -O3                # Optimization level 3
///   lucid run main.luc --entry init       # Use 'init' as entry point
///
/// ─── Exit Codes ──────────────────────────────────────────────────────────────
///
///   0 : Success (no errors)
///   1 : Error (file not found, parse error, semantic error, etc.)
///
/// ─── Debug Build Commands ────────────────────────────────────────────────────
///
/// When built with debug flags (see CMakeLists.txt), additional logging
/// is available:
///
///   cmake -B build -DDEBUG_PARSER=ON -DDEBUG_SEMANTIC=ON
///   cmake --build build
///   ./build/lucid-comp parse myfile.luc --verbose
///
/// Available debug flags:
///   - DEBUG_MASTER      : Enable all debug logging
///   - DEBUG_LEXER       : Enable lexer debug logging
///   - DEBUG_PARSER      : Enable parser debug logging
///   - DEBUG_TYPE        : Enable type parsing debug logging
///   - DEBUG_SEMANTIC    : Enable semantic analysis debug logging
///   - DEBUG_INTERPRETER : Enable interpreter debug logging
///   - DEBUG_CODEGEN     : Enable code generation debug logging
///   - DEBUG_VERBOSITY   : Output detail level (0=MINIMAL, 1=NORMAL, 2=DETAIL)
///   - DEBUG_TO_FILE     : Write logs to file instead of stdout
///   - DEBUG_FILE_PATH   : Custom log file path
///
/// @see CLIOptions.hpp for option definitions
/// @see Pipeline.hpp for pipeline implementation
/// @see run.hpp for run command implementation
/// @see parse.hpp for parse command implementation
/// @see sema.hpp for sema command implementation

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <string>

#include "cli/CLIOptions.hpp"
#include "cli/run.hpp"
#include "cli/frontend/parse.hpp"
#include "cli/frontend/sema.hpp"
#include "interpreter/support/InterpreterOptions.hpp"

/**
 * @brief Print the usage/help message to stdout.
 *
 * Displays all available commands and their options in a formatted
 * help screen. Called when:
 *   - User passes --help or -h
 *   - No arguments are provided
 *   - An unknown command is entered
 *
 * The output is organized into sections:
 *   1. Version information
 *   2. Available commands with descriptions
 *   3. Run command options (--verbose, --no-hot-reload, -O, --entry)
 *   4. Parse/Sema command options (--json, --json-pretty, -o, --verbose)
 *   5. Build command options (--emit-llvm, -o, --target)
 *   6. Generic options (--help)
 */
void printUsage() {
    std::cout << "Lucid Compiler v0.1.0\n\n"
              << "Usage:\n"
              << "  lucid run   <file.luc> [options]   -- JIT interpret and execute\n"
              << "  lucid parse <file.luc> [options]   -- Parse only (stop after AST)\n"
              << "  lucid sema  <file.luc> [options]   -- Parse + semantic analysis\n"
              << "  lucid build <file.luc> [options]   -- AOT compile to native binary\n"
              << "  lucid repl                         -- Interactive REPL\n\n"
              << "Run options:\n"
              << "  --verbose            Enable verbose output\n"
              << "  --no-hot-reload      Disable hot-reload (file watcher)\n"
              << "  -O<level>            Optimization level (0-3, default: 2)\n"
              << "  --entry <name>       Entry point function name (default: main)\n"
              << "  --help               Show this help message\n\n"
              << "Parse/Sema options:\n"
              << "  --json               Output as JSON (machine-readable)\n"
              << "  --json-pretty        Output as pretty JSON (human-readable)\n"
              << "  -o <file>            Write output to file (instead of stdout)\n"
              << "  --verbose            Show parsing/semantic progress\n\n"
              << "Build options (future):\n"
              << "  -o <file>            Output file name (default: a.out)\n"
              << "  --emit-llvm          Emit LLVM IR instead of native code\n"
              << "  --target <triple>    Target triple (default: host)\n";
}

/**
 * @brief Main entry point for the Lucid Compiler CLI.
 *
 * ─── Command Processing Flow ────────────────────────────────────────────────
 *
 * 1. **Argument Parsing**:
 *    - First argument is the command (run, parse, sema, build, repl)
 *    - Remaining arguments are parsed into CLIOptions
 *    - Flags like --verbose, --json, etc. are stored
 *    - Positional arguments are stored as rootFilePath and programArgs
 *
 * 2. **Command Dispatch**:
 *    - `run`   → cli::runCommand()        (full execution)
 *    - `parse` → cli::frontend::parseCommand() (stop after AST)
 *    - `sema`  → cli::frontend::semaCommand()  (stop after semantic analysis)
 *    - `build` → Not yet implemented (future)
 *    - `repl`  → Not yet implemented (future)
 *
 * 3. **Pipeline Control**:
 *    - Each command sets a different `stopAt` value
 *    - The pipeline stops at the specified stage
 *    - Intermediate results can be dumped (--json, --json-pretty)
 *
 * ─── Example Command Parsing ────────────────────────────────────────────────
 *
 * Command: lucid sema main.luc --json-pretty -o ast.json
 *
 * argv = ["lucid", "sema", "main.luc", "--json-pretty", "-o", "ast.json"]
 *   command = "sema"
 *   opts.rootFilePath = "main.luc"
 *   opts.outputFormat = OutputFormat::JsonPretty
 *   opts.outputFile = "ast.json"
 *   opts.stopAt = PipelineStage::Sema
 *   opts.command = CLIOptions::Command::Sema
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings.
 * @return Exit code: 0 on success, 1 on error.
 */
int main(int argc, char* argv[]) {
    // ─── Debug Startup ─────────────────────────────────────────────────────
    // If the compiler was built with DEBUG_MASTER=ON, print a confirmation
    // message. This helps verify that debug flags are correctly applied.
    #ifdef LUC_DEBUG_MASTER
        std::cout << "[DEBUG] LUC_DEBUG_MASTER is ENABLED" << std::endl;
    #endif

    // ─── No Arguments ──────────────────────────────────────────────────────
    // If no arguments are provided, show the help message and exit.
    if (argc < 2) {
        printUsage();
        return 1;
    }

    // ─── Parse Command Name ───────────────────────────────────────────────
    // The first argument is always the sub-command.
    std::string command = argv[1];

    // ─── Parse Options ─────────────────────────────────────────────────────
    // Initialize default options and then override with user-provided values.
    cli::CLIOptions opts;
    opts.command = cli::CLIOptions::Command::Unknown;
    opts.outputFormat = cli::OutputFormat::Text;  // Default

    // Iterate through all arguments starting from index 2 (after the command).
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        // ─── Help / Version ──────────────────────────────────────────────
        // Show help and exit immediately.
        if (arg == "--help" || arg == "-h") {
            opts.showHelp = true;
            printUsage();
            return 0;
        }

        // ─── Verbose Output ──────────────────────────────────────────────
        // Enables detailed logging of all compiler phases.
        if (arg == "--verbose") {
            opts.verbose = true;
            continue;
        }

        // ─── Hot-Reload ──────────────────────────────────────────────────
        // Disables the file watcher that automatically recompiles on changes.
        if (arg == "--no-hot-reload") {
            opts.enableHotReload = false;
            continue;
        }

        // ─── JSON Output ─────────────────────────────────────────────────
        // Machine-readable JSON output (compact).
        if (arg == "--json") {
            opts.outputFormat = cli::OutputFormat::Json;
            continue;
        }

        // ─── Pretty JSON Output ──────────────────────────────────────────
        // Human-readable JSON output with indentation.
        if (arg == "--json-pretty") {
            opts.outputFormat = cli::OutputFormat::JsonPretty;
            continue;
        }

        // ─── Output File ──────────────────────────────────────────────────
        // Write output to a file instead of stdout.
        if (arg == "-o" && i + 1 < argc) {
            opts.outputFile = argv[++i];
            continue;
        }

        // ─── Optimization Level ──────────────────────────────────────────
        // Sets the LLVM optimization level for code generation.
        // Valid values: 0 (none), 1 (basic), 2 (default), 3 (aggressive)
        if (arg.rfind("-O", 0) == 0) {
            std::string level = arg.substr(2);
            opts.interpreter.optimizationLevel = std::stoi(level);
            continue;
        }

        // ─── Entry Point ─────────────────────────────────────────────────
        // Overrides the default entry point function name ("main").
        if (arg == "--entry" && i + 1 < argc) {
            opts.entryPoint = argv[++i];
            continue;
        }

        // ─── Positional Arguments ────────────────────────────────────────
        // - The first non-flag argument is treated as the root file path.
        // - Additional positional arguments are stored as program args.
        if (opts.rootFilePath.empty() && arg[0] != '-') {
            opts.rootFilePath = arg;
        } else if (arg[0] != '-') {
            opts.programArgs.push_back(arg);
        }
    }

    // ─── Dispatch Commands ─────────────────────────────────────────────────
    // Route the request to the appropriate command handler based on the
    // first argument (the sub-command).
    
    // ─── Run Command ──────────────────────────────────────────────────────
    // JIT compiles and executes the specified Lucid file.
    // Pipeline stops at: Execute (full execution)
    // Requires: rootFilePath (the file to execute)
    if (command == "run") {
        if (opts.rootFilePath.empty()) {
            std::cerr << "Error: No file specified for 'run' command.\n";
            return 1;
        }
        opts.command = cli::CLIOptions::Command::Run;
        opts.stopAt = cli::PipelineStage::Execute;
        return cli::runCommand(opts);
    }

    // ─── Parse Command ────────────────────────────────────────────────────
    // Parses the file and stops after AST construction.
    // Useful for: syntax checking, parser debugging, AST analysis
    // Pipeline stops at: Parse (after AST)
    // Requires: rootFilePath (the file to parse)
    // Options: --json, --json-pretty, -o, --verbose
    if (command == "parse") {
        if (opts.rootFilePath.empty()) {
            std::cerr << "Error: No file specified for 'parse' command.\n";
            return 1;
        }
        opts.command = cli::CLIOptions::Command::Parse;
        opts.stopAt = cli::PipelineStage::Parse;
        return cli::frontend::parseCommand(opts);
    }

    // ─── Sema Command ─────────────────────────────────────────────────────
    // Parses the file and runs semantic analysis, stopping after type checking.
    // Useful for: type error checking, semantic debugging, AST validation
    // Pipeline stops at: Sema (after semantic analysis)
    // Requires: rootFilePath (the file to analyze)
    // Options: --json, --json-pretty, -o, --verbose
    if (command == "sema") {
        if (opts.rootFilePath.empty()) {
            std::cerr << "Error: No file specified for 'sema' command.\n";
            return 1;
        }
        opts.command = cli::CLIOptions::Command::Sema;
        opts.stopAt = cli::PipelineStage::Sema;
        return cli::frontend::semaCommand(opts);
    }

    // ─── Build Command ────────────────────────────────────────────────────
    // Ahead-of-time compiles to a native binary.
    // Not yet implemented (placeholder for future development).
    if (command == "build") {
        opts.command = cli::CLIOptions::Command::Build;
        std::cerr << "Error: 'build' command not yet implemented.\n";
        return 1;
    }

    // ─── REPL Command ─────────────────────────────────────────────────────
    // Interactive read-eval-print loop for experimenting with Lucid code.
    // Not yet implemented (placeholder for future development).
    if (command == "repl") {
        opts.command = cli::CLIOptions::Command::Repl;
        std::cerr << "Error: 'repl' command not yet implemented.\n";
        return 1;
    }

    // ─── Unknown Command ──────────────────────────────────────────────────
    // If the command doesn't match any known sub-command, show help.
    std::cerr << "Error: Unknown command '" << command << "'\n";
    std::cerr << "Valid commands: run, parse, sema, build, repl\n";
    std::cerr << "Use --help for more information.\n";
    printUsage();
    return 1;
}