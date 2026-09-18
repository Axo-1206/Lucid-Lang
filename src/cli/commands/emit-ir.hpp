/// @file cli/commands/emit-ir.hpp
/// @brief 'lucid emit-ir' command - emit LLVM IR as text.

#pragma once

#include "../CLIOptions.hpp"

namespace cli::commands {

/// @brief Execute the 'lucid emit-ir' command.
/// 
/// Parses the file, runs semantic analysis, generates LLVM IR, and
/// writes the IR as text to stdout or to the path given by -o.
/// 
/// ─── Implementation ──────────────────────────────────────────────────
/// The pipeline runs through parse, sema, and IR generation, then
/// serializes the resulting llvm::Module(s) to text. The pipeline owns
/// a temporary llvm::LLVMContext for the run and discards it once the
/// text is extracted — see Pipeline.cpp's CodeGen stage.
/// 
/// ─── Output ────────────────────────────────────────────────────────────
/// - Without -o: IR is written to stdout.
/// - With -o <file>: IR is written to <file>, truncating any existing
///   contents. Parent directories are not created; the caller must
///   ensure the path is writable.
/// 
/// @param opts CLI options (rootFilePath is required)
/// @return 0 on success, 1 if any pipeline stage failed
int emitIRCommand(const CLIOptions& opts);

} // namespace cli::commands