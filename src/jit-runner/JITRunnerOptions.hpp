/// @file jit-runner/JITRunnerOptions.hpp
/// @brief Configuration options for the JIT runner.

#pragma once

#include <string>
#include <vector>

namespace jit_runner {

/// @brief Options for the JIT runner.
///
/// These control the runner's behavior — the JIT's optimization level,
/// verbose logging, and diagnostic-relevant settings. They are not CLI
/// options; the CLI populates this struct and passes it in.
struct JITRunnerOptions {
    /// LLVM optimization level for the JIT: 0 through 3.
    /// 0 = no optimization; 3 = aggressive. Default 2 matches `clang -O2`.
    int optimizationLevel = 2;

    /// Emit verbose progress output to stderr.
    bool verbose = false;

    /// Path to the kernel library to preload, if any. Empty means none.
    /// The runner loads this before any manifest-declared foreign
    /// library, so kernel symbols are available to every module.
    std::string kernelLibraryPath;

    /// Extra directories to search for foreign libraries. Prepended to
    /// the platform linker's default search path.
    std::vector<std::string> librarySearchPaths;
};

} // namespace jit_runner