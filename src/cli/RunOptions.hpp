/// @file cli/RunOptions.hpp
/// @brief Options for the 'run' command.

#pragma once

#include <string>
#include <vector>

namespace cli {

/**
 * @brief Command-line options for `lucid run`.
 */
struct RunOptions {
    /// @brief Entry point function name (default: "main").
    std::string entryPoint = "main";

    /// @brief Enable verbose output.
    bool verbose = false;

    /// @brief Enable hot‑reload (file watcher).
    bool enableHotReload = true;

    /// @brief Optimization level (0-3).
    int optimizationLevel = 2;

    /// @brief Enable debug info in JIT.
    bool enableDebugInfo = true;

    /// @brief Extra arguments passed to the program.
    std::vector<std::string> programArgs;

    /// @brief Time (in seconds) before the file watcher starts.
    int watchDelaySeconds = 1;
};

} // namespace cli