#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <filesystem>
#include <unordered_set>

#include "cli/RunOptions.hpp"
#include "cli/run.hpp"

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
    // ========================================================================
    // DEBUG INITIALISATION
    // ========================================================================
    #ifdef LUC_DEBUG_MASTER
        std::cout << "[DEBUG] LUC_DEBUG_MASTER is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_PARSER
        std::cout << "[DEBUG] LUC_DEBUG_PARSER is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_TYPE
        std::cout << "[DEBUG] LUC_DEBUG_TYPE is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_SEMANTIC
        std::cout << "[DEBUG] LUC_DEBUG_SEMANTIC is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_DUMP_SYMBOL
        std::cout << "[DEBUG] LUC_DEBUG_DUMP_SYMBOL is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_PARSE_RESULT
        std::cout << "[DEBUG] LUC_DEBUG_PARSE_RESULT is ENABLED" << std::endl;
    #endif
    #ifdef LUC_DEBUG_TO_FILE
        std::cout << "[DEBUG] LUC_DEBUG_TO_FILE is ENABLED" << std::endl;
        std::cout << "[DEBUG] Log file path: " << getAbsolutePath(LUC_DEBUG_FILE_PATH) << std::endl;
    #endif
    #ifdef LUC_DEBUG_VERBOSITY
        std::cout << "[DEBUG] LUC_DEBUG_VERBOSITY = " << LUC_DEBUG_VERBOSITY << std::endl;
    #endif

    std::string command = argv[1];

    // ─── Parse common options ──────────────────────────────────────────
    cli::RunOptions runOptions;
    std::string filePath;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }

        if (arg == "--verbose") {
            runOptions.verbose = true;
            continue;
        }

        if (arg == "--no-hot-reload") {
            runOptions.enableHotReload = false;
            continue;
        }

        if (arg.rfind("-O", 0) == 0) {
            std::string level = arg.substr(2);
            runOptions.optimizationLevel = std::stoi(level);
            continue;
        }

        if (arg == "--entry" && i + 1 < argc) {
            runOptions.entryPoint = argv[++i];
            continue;
        }

        // Assume it's the file path
        if (filePath.empty() && arg[0] != '-') {
            filePath = arg;
        } else {
            // Program arguments
            runOptions.programArgs.push_back(arg);
        }
    }

    // ─── Dispatch commands ──────────────────────────────────────────────
    if (command == "run") {
        if (filePath.empty()) {
            std::cerr << "Error: No file specified for 'run' command.\n";
            return 1;
        }
        return cli::runCommand(filePath, runOptions);
    }

    if (command == "build") {
        std::cerr << "Error: 'build' command not yet implemented.\n";
        return 1;
    }

    if (command == "repl") {
        std::cerr << "Error: 'repl' command not yet implemented.\n";
        return 1;
    }

    std::cerr << "Error: Unknown command '" << command << "'\n";
    printUsage();
    return 1;

#ifdef LUC_DEBUG_TO_FILE
    LucDebug::getDebugStream() << std::flush;
    std::cout << "[MAIN] Debug logs written to: " << getAbsolutePath(LUC_DEBUG_FILE_PATH) << std::endl;
#endif

    return 0;
}