/// @file execution/SymbolResolver.hpp
/// @brief Resolves symbols and entry points in loaded modules.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../core/InterpreterContext.hpp"
#include <vector>
#include <string>

namespace interpreter {

/// @brief Resolves symbols and entry points.
class SymbolResolver {
public:
    explicit SymbolResolver(InterpreterContext& ctx);
    ~SymbolResolver() = default;

    /// @brief Find the entry point in loaded modules.
    /// @param entryPoint The suggested entry point name (e.g., "main").
    /// @return The found entry point name, or empty if not found.
    InternedString findEntryPoint(InternedString entryPoint);

    /// @brief Find the entry point using a string.
    InternedString findEntryPoint(const std::string& entryPoint);

    /// @brief Check if a function is exported.
    bool isExported(const FuncDeclAST* func) const;

    /// @brief Check if a function is an entry point candidate.
    bool isEntryPointCandidate(const FuncDeclAST* func) const;

    /// @brief Get all entry point candidates.
    std::vector<const FuncDeclAST*> getEntryPointCandidates() const;

    /// @brief Look up a symbol by name.
    /// @param name The symbol name (mangled or unmangled).
    /// @return Pointer to the symbol, or nullptr if not found.
    void* lookupSymbol(const std::string& name);

    /// @brief Look up a symbol by InternedString.
    void* lookupSymbol(InternedString name);

private:
    InterpreterContext& m_ctx;

    /// @brief Get the mangled name for a function.
    InternedString getMangledName(const FuncDeclAST* func) const;
};

} // namespace interpreter