/// @file support/InterpreterOptions.hpp
/// @brief Configuration options for the interpreter.

#pragma once

#include <string>
#include <vector>

namespace interpreter {

/// @brief Options for configuring the interpreter.
struct InterpreterOptions {
    /// Enable LLVM optimizations (default: true)
    bool enableOptimizations = true;
    
    /// Enable debug info generation (default: false)
    bool enableDebugInfo = false;
    
    /// Optimization level: 0-3 (default: 2)
    int optimizationLevel = 2;
    
    /// Default entry point name (default: "main")
    std::string entryPoint = "main";
    
    /// Additional library search paths
    std::vector<std::string> libraryPaths;
    
    /// Enable hot-reload support (default: false)
    bool enableHotReload = false;
    
    /// Enable verbose output (default: false)
    bool verbose = false;
    
    /// Enable JIT profiling (default: false)
    bool enableProfiling = false;
    
    /// Path to the kernel library (platform-specific default)
    std::string kernelLibraryPath;
};

} // namespace interpreter