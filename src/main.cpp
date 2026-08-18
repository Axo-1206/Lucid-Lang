#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>

#include "cli/CLIOptions.hpp"
#include "cli/run.hpp"
#include "interpreter/support/InterpreterOptions.hpp"

void printUsage() {
    std::cout << "Lucid Compiler v0.1.0\n\n"
              << "Usage:\n"
              << "  lucid run <file.luc> [options]   -- JIT interpret and execute\n"
              << "  lucid build <file.luc> [options] -- AOT compile to native binary\n"
              << "  lucid repl                        -- Interactive REPL\n\n"
              << "Run options:\n"
              << "  --verbose            Enable verbose output\n"
              << "  --no-hot-reload      Disable hot-reload (file watcher)\n"
              << "  -O<level>            Optimization level (0-3, default: 2)\n"
              << "  --entry <name>       Entry point function name (default: main)\n"
              << "  --help               Show this help message\n";
}

int main(int argc, char* argv[]) {
    // ─── Debug initialization ──────────────────────────────────────────
    #ifdef LUC_DEBUG_MASTER
        std::cout << "[DEBUG] LUC_DEBUG_MASTER is ENABLED" << std::endl;
    #endif

    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string command = argv[1];

    // ─── Parse options ──────────────────────────────────────────────────
    cli::CLIOptions opts;
    opts.command = cli::CLIOptions::Command::Unknown;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            opts.showHelp = true;
            printUsage();
            return 0;
        }

        if (arg == "--verbose") {
            opts.verbose = true;
            continue;
        }

        if (arg == "--no-hot-reload") {
            opts.enableHotReload = false;
            continue;
        }

        if (arg.rfind("-O", 0) == 0) {
            std::string level = arg.substr(2);
            opts.interpreter.optimizationLevel = std::stoi(level);
            continue;
        }

        if (arg == "--entry" && i + 1 < argc) {
            opts.entryPoint = argv[++i];
            continue;
        }

        if (opts.rootFilePath.empty() && arg[0] != '-') {
            opts.rootFilePath = arg;
        } else if (arg[0] != '-') {
            opts.programArgs.push_back(arg);
        }
    }

    // ─── Dispatch commands ──────────────────────────────────────────────
    if (command == "run") {
        if (opts.rootFilePath.empty()) {
            std::cerr << "Error: No file specified for 'run' command.\n";
            return 1;
        }
        opts.command = cli::CLIOptions::Command::Run;
        return cli::runCommand(opts);
    }

    if (command == "build") {
        opts.command = cli::CLIOptions::Command::Build;
        std::cerr << "Error: 'build' command not yet implemented.\n";
        return 1;
    }

    if (command == "repl") {
        opts.command = cli::CLIOptions::Command::Repl;
        std::cerr << "Error: 'repl' command not yet implemented.\n";
        return 1;
    }

    std::cerr << "Error: Unknown command '" << command << "'\n";
    printUsage();
    return 1;
}