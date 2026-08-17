/// @file dynlink/DynamicLinker.hpp
/// @brief Platform-agnostic dynamic library loader.

#pragma once

#include "LibraryHandle.hpp"
#include "core/memory/InternedString.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"
#include <unordered_map>
#include <vector>
#include <string>

namespace interpreter {

class JITSession;

/// @brief Platform-agnostic dynamic library loader.
class DynamicLinker {
public:
    DynamicLinker() = default;
    ~DynamicLinker() = default;

    DynamicLinker(const DynamicLinker&) = delete;
    DynamicLinker& operator=(const DynamicLinker&) = delete;
    DynamicLinker(DynamicLinker&&) = default;
    DynamicLinker& operator=(DynamicLinker&&) = default;

    // ─── Library Loading ──────────────────────────────────────────────

    bool load(const std::string& name);
    bool loadPath(const std::string& path);
    bool unload(const std::string& name);

    // ─── Library Registration ─────────────────────────────────────────

    /// @brief Register libraries from a module's @[link] attributes.
    void registerLibrariesFromModule(DiagnosticEngine& diagnostics,
                                     StringPool& pool,
                                     bool verbose,
                                     ModuleAST* module);

    /// @brief Register libraries from multiple modules.
    void registerLibrariesFromModules(DiagnosticEngine& diagnostics,
                                      StringPool& pool,
                                      bool verbose,
                                      const std::vector<ModuleAST*>& modules);

    // ─── Symbol Lookup ────────────────────────────────────────────────

    void* getSymbol(const std::string& name) const;
    void* getSymbol(InternedString name) const;
    std::unordered_map<std::string, void*> getAllSymbols() const;
    std::unordered_map<std::string, void*> getLibrarySymbols(
        const std::string& name) const;

    // ─── JIT Integration ──────────────────────────────────────────────

    void registerWithJIT(JITSession& jit);
    bool registerLibraryWithJIT(JITSession& jit, const std::string& name);

    // ─── Query ─────────────────────────────────────────────────────────

    bool isLoaded(const std::string& name) const;
    std::vector<std::string> getLoadedLibraries() const;

private:
    std::unordered_map<std::string, std::unique_ptr<LibraryHandle>> m_libraries;
    mutable std::unordered_map<std::string, void*> m_symbolCache;
    mutable bool m_cacheDirty = true;

    std::string getLibraryFileName(const std::string& name) const;
    std::string getLibraryPath(const std::string& name) const;
    void rebuildCache() const;
};

} // namespace interpreter