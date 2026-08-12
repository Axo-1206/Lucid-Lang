/// @file support/ExecutionResult.hpp
/// @brief Result of executing a module.

#pragma once

#include <string>

namespace interpreter {

/// @brief Result of executing a program.
struct ExecutionResult {
    /// Exit code from the entry point (0 = success)
    int exitCode = 0;
    
    /// Whether execution succeeded
    bool success = true;
    
    /// Error message if execution failed
    std::string errorMessage;
    
    /// Execution time in milliseconds
    double executionTimeMs = 0.0;
    
    /// The entry point that was executed
    std::string entryPointUsed;
    
    /// Whether the execution was interrupted (e.g., by a panic)
    bool interrupted = false;
};

} // namespace interpreter