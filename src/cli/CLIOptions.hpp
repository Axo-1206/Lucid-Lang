/// @file cli/CLIOptions.hpp
/// @brief Unified CLI options for all commands.

#pragma once

#include "interpreter/support/InterpreterOptions.hpp"

#include <string>
#include <vector>

namespace cli {

/**
 * @brief Unified CLI options for all commands.
 *
 * This struct contains:
 *   - CLI-specific options (file watcher, program args, etc.)
 *   - A nested InterpreterOptions for JIT configuration
 *
 * When the interpreter is initialized, the CLI-specific fields
 * (verbose, entryPoint, enableHotReload) override the interpreter's
 * defaults if explicitly set.
 */
struct CLIOptions {
    // ─── Command ──────────────────────────────────────────────────────
    enum class Command {
        Unknown,
        Run,
        Build,
        Repl,
    } command = Command::Unknown;

    // ─── CLI-Specific (File Watcher, Program Args, etc.) ────────────

    /// @brief CLI verbose output (overrides interpreter.verbose if true).
    bool verbose = false;

    /// @brief Show help message.
    bool showHelp = false;

    /// @brief Entry point function name (overrides interpreter.entryPoint).
    std::string entryPoint = "main";

    /// @brief Extra arguments passed to the program (run command only).
    std::vector<std::string> programArgs;

    /// @brief Enable hot-reload file watcher (CLI feature).
    bool enableHotReload = true;

    /// @brief Delay before starting file watcher (seconds).
    int watchDelaySeconds = 1;

    /// @brief Path to the root file (set by CLI, not user).
    std::string rootFilePath;

    /// @brief Output file (build command only).
    std::string outputFile = "a.out";

    /// @brief Emit LLVM IR instead of native code (build command).
    bool emitLLVM = false;

    /// @brief Target triple (empty = host) (build command).
    std::string targetTriple;

    /// @brief REPL prompt string.
    std::string replPrompt = "> ";

    /// @brief REPL init file.
    std::string replInitFile;

    // ─── Interpreter Options ──────────────────────────────────────────

    /// @brief Nested interpreter options (JIT, optimization, etc.)
    interpreter::InterpreterOptions interpreter;
};

} // namespace cli