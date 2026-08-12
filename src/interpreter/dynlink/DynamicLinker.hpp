/// @file dynlink/DynamicLinker.hpp
/// @brief Platform-agnostic dynamic library loader.

#pragma once

#include "LibraryHandle.hpp"
#include "core/memory/InternedString.hpp"
#include <unordered_map>
#include <vector>
#include <string>

namespace interpreter {

class JITSession;

/// @brief Platform-agnostic dynamic library loader.
///
/// Manages loading of shared libraries and their symbols.
/// Provides symbol lookup across all loaded libraries.
class DynamicLinker {
public:
    DynamicLinker() = default;
    ~DynamicLinker() = default;

    // Non-copyable
    DynamicLinker(const DynamicLinker&) = delete;
    DynamicLinker& operator=(const DynamicLinker&) = delete;

    // Moveable
    DynamicLinker(DynamicLinker&&) = default;
    DynamicLinker& operator=(DynamicLinker&&) = default;

    /// @brief Load a library by name (searches system paths).
    /// @param name The library name (e.g., "opengl", "m").
    /// @return true on success.
    /// @throws InterpreterError if loading fails.
    bool load(const std::string& name);

    /// @brief Load a library from a specific path.
    /// @param path The full path to the library.
    /// @return true on success.
    /// @throws InterpreterError if loading fails.
    bool loadPath(const std::string& path);

    /// @brief Unload a previously loaded library.
    /// @param name The library name.
    /// @return true if the library was found and unloaded.
    bool unload(const std::string& name);

    /// @brief Get a symbol from any loaded library.
    /// @param name The symbol name.
    /// @return Pointer to the symbol, or nullptr if not found.
    void* getSymbol(const std::string& name) const;

    /// @brief Get a symbol by InternedString.
    void* getSymbol(InternedString name) const;

    /// @brief Get all symbols from all loaded libraries.
    std::unordered_map<std::string, void*> getAllSymbols() const;

    /// @brief Get symbols from a specific library.
    std::unordered_map<std::string, void*> getLibrarySymbols(
        const std::string& name) const;

    /// @brief Register all loaded libraries with the JIT.
    void registerWithJIT(JITSession& jit);

    /// @brief Register a specific library with the JIT.
    bool registerLibraryWithJIT(JITSession& jit, const std::string& name);

    /// @brief Check if a library is loaded.
    bool isLoaded(const std::string& name) const;

    /// @brief Get the list of loaded library names.
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