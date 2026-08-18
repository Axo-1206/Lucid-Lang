/// @file cli/CLIContext.hpp
/// @brief Shared CLI context for a single run session.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/memory/ASTArena.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <filesystem>

namespace cli {

/**
 * @brief Shared context for a CLI run session.
 *
 * Holds all infrastructure that lives for the duration of a single
 * `lucid run` or `lucid build` command.
 *
 * @note The ASTArena holds all AST nodes for the entire program.
 *       It is NOT reset between hot-reload cycles — new ASTs are
 *       allocated alongside old ones. Old ASTs remain in memory
 *       but are no longer referenced after the interpreter updates
 *       its ModuleRegistry.
 */
struct CLIContext {
    StringPool stringPool;
    DiagnosticEngine diagnostics;
    ASTArena astArena;  // Holds ALL ASTs for the program (including old versions)
    std::filesystem::path packageRoot;

    CLIContext() = default;
    CLIContext(const std::filesystem::path& root)
        : packageRoot(root) {}

    // Non-copyable
    CLIContext(const CLIContext&) = delete;
    CLIContext& operator=(const CLIContext&) = delete;
};

} // namespace cli