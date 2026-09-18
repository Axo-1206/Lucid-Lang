/// @file cli/CLIOptions.hpp
/// @brief Unified CLI options for all commands.

#pragma once

#include "interpreter/support/InterpreterOptions.hpp"

#include <string>
#include <vector>
#include <optional>

namespace cli {

/// @brief Pipeline stages where execution can stop.
enum class PipelineStage {
    /// Stop after lexing (tokens only)
    Lex,
    /// Stop after parsing (AST only)
    Parse,
    /// Stop after semantic analysis (validated AST)
    Sema,
    /// Stop after LLVM IR generation, keeping the IR in memory.
    CodeGen,
    /// Stop after LLVM IR generation, with the IR serialized to text
    /// for the `emit-ir` command.
    ///
    /// CodeGen and EmitIR currently run the same codegen pass and
    /// differ only in whether the caller intends to consume the IR as
    /// text. Keeping them distinct lets that divergence grow without
    /// renaming an enum value later.
    EmitIR,
    /// Full execution (default for 'run')
    Execute,
    /// AOT compilation
    Build,
};

/// @brief Output formats for structured data.
enum class OutputFormat {
    /// Human-readable text with emojis and formatting (default)
    Text,
    /// JSON for machine consumption (LSP, tools)
    Json,
    /// Pretty JSON with indentation (for human reading)
    JsonPretty,
};

/// @brief Unified CLI options for all commands.
struct CLIOptions {
    // ─── Command ──────────────────────────────────────────────────────
    enum class Command {
        Unknown,
        Run,        // Full execution (default)
        Parse,      // Parse only (stop after AST)
        Sema,       // Parse + semantic analysis
        CodeGen,    // Parse + sema + LLVM IR generation (internal)
        EmitIR,     // Parse + sema + IR generation, write IR as text
        Build,      // AOT compile to native binary
        Repl,       // Interactive REPL
    } command = Command::Unknown;

    // ─── Pipeline Control ────────────────────────────────────────────
    
    /// @brief Stage to stop at (default depends on command).
    PipelineStage stopAt = PipelineStage::Execute;
    
    /// @brief Output format for structured data.
    OutputFormat outputFormat = OutputFormat::Text;

    // ─── CLI-Specific Options ────────────────────────────────────────

    /// @brief CLI verbose output (overrides interpreter.verbose if true).
    bool verbose = false;

    /// @brief Enable detailed tracing output (shows compilation progress).
    bool trace = false;

    /// @brief Show help message.
    bool showHelp = false;

    /// @brief Entry point function name.
    std::string entryPoint = "main";

    /// @brief Extra arguments passed to the program (run command only).
    std::vector<std::string> programArgs;

    /// @brief Enable hot-reload file watcher (CLI feature).
    bool enableHotReload = true;

    /// @brief Delay before starting file watcher (seconds).
    int watchDelaySeconds = 1;

    /// @brief Path to the root file (set by CLI, not user).
    std::string rootFilePath;

    /// @brief Output file for structured data (parse/sema commands).
    std::optional<std::string> outputFile;

    /// @brief Output file (build command only).
    std::string buildOutputFile = "a.out";

    /// @brief Target triple (empty = host) (build command).
    std::string targetTriple;

    /// @brief REPL prompt string.
    std::string replPrompt = "> ";

    /// @brief REPL init file.
    std::string replInitFile;

    // ─── Parse/Sema Command Specific ────────────────────────────────
    
    /// @brief Dump the AST to stdout (parse/sema commands).
    bool dumpAST = false;

    // ─── Interpreter Options ──────────────────────────────────────────

    /// @brief Nested interpreter options (JIT, optimization, etc.)
    interpreter::InterpreterOptions interpreter;
};

} // namespace cli