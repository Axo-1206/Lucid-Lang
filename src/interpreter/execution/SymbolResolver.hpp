/// @file execution/SymbolResolver.hpp
/// @brief Symbol resolution functions - procedural style.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../core/InterpreterContext.hpp"
#include <vector>
#include <string>

namespace interpreter {

// ─── Entry Point Resolution ─────────────────────────────────────────────

/// @brief Find the entry point in loaded modules.
/// @param ctx The interpreter context.
/// @param entryPoint The suggested entry point name (e.g., "main").
/// @return The found entry point name, or empty if not found.
InternedString findEntryPoint(InterpreterContext& ctx, InternedString entryPoint);

/// @brief Find the entry point using a string.
/// @param ctx The interpreter context.
/// @param entryPoint The suggested entry point name.
/// @return The found entry point name, or empty if not found.
InternedString findEntryPoint(InterpreterContext& ctx, const std::string& entryPoint);

/// @brief Check if a function is exported (@[export] attribute).
/// @param func The function declaration.
/// @param ctx The interpreter context.
/// @return true if the function has @[export].
bool isExported(const FuncDeclAST* func, InterpreterContext& ctx);

/// @brief Check if a function is an entry point candidate.
/// @param func The function declaration.
/// @param ctx The interpreter context.
/// @return true if the function is a valid entry point.
bool isEntryPointCandidate(const FuncDeclAST* func, InterpreterContext& ctx);

/// @brief Get all entry point candidates from loaded modules.
/// @param ctx The interpreter context.
/// @return Vector of candidate function declarations.
std::vector<const FuncDeclAST*> getEntryPointCandidates(InterpreterContext& ctx);

// ─── Name Mangling ──────────────────────────────────────────────────────

/// @brief Get the mangled name for a function.
/// @param func The function declaration.
/// @param ctx The interpreter context.
/// @return The mangled name (or the original name if not mangled).
InternedString getMangledName(const FuncDeclAST* func, InterpreterContext& ctx);

} // namespace interpreter