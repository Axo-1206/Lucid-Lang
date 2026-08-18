/// @file support/InterpreterOptions.hpp
/// @brief Configuration options for the interpreter.

#pragma once

#include <string>
#include <vector>

namespace interpreter {

/**
 * @brief Options for configuring the interpreter (JIT) engine.
 *
 * These options control LLVM JIT behavior, not CLI behavior.
 * They are used by the interpreter regardless of whether it's
 * called from CLI, LSP, or an embedded environment.
 */
struct InterpreterOptions {
    /// @brief Enable LLVM optimizations.
    bool enableOptimizations = true;

    /// @brief Enable debug info generation in JIT.
    bool enableDebugInfo = false;

    /// @brief Optimization level: 0-3.
    int optimizationLevel = 2;

    /// @brief Enable hot-reload support (JIT module replacement).
    bool enableHotReload = false;  // <-- ADD THIS

    /// @brief Enable verbose output from the interpreter.
    bool verbose = false;

    /// @brief Enable JIT profiling.
    bool enableProfiling = false;

    /// @brief Path to the kernel library (platform-specific default).
    std::string kernelLibraryPath;

    /// @brief Additional library search paths.
    std::vector<std::string> libraryPaths;

    /// @brief Default entry point name.
    std::string entryPoint = "main";
};

} // namespace interpreter