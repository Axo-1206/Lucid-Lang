/// @file cli/commands/parse.hpp
/// @brief 'lucid parse' command - parse-only mode for debugging.

#pragma once

#include "../CLIOptions.hpp"

namespace lucid::cli::commands {

/// @brief Execute the 'lucid parse' command.
/// 
/// Parses the file and stops after AST construction.
/// Optionally dumps the AST with --dump-ast.
/// 
/// @param opts CLI options (rootFilePath is required)
/// @return 0 on success (no errors), 1 if parsing failed
int parseCommand(const CLIOptions& opts);

} // namespace lucid::cli::commands