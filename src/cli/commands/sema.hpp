/// @file cli/commands/sema.hpp
/// @brief 'lucid sema' command - parse + semantic analysis.

#pragma once

#include "../CLIOptions.hpp"

namespace cli::commands {

/// @brief Execute the 'lucid sema' command.
/// 
/// Parses the file and runs semantic analysis.
/// Optionally dumps the validated AST with --dump-ast.
/// 
/// @param opts CLI options (rootFilePath is required)
/// @return 0 on success (no errors), 1 if semantic analysis failed
int semaCommand(const CLIOptions& opts);

} // namespace cli::commands