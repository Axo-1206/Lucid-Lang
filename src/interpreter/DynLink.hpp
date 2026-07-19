/**
 * @file DynLink.hpp
 * @brief Platform-agnostic dynamic library loader for the Lucid interpreter.
 * 
 * @responsibility Handles loading of shared libraries (.dll/.so/.dylib),
 *                 extracting symbols, and registering them with the JIT.
 * 
 * @related_files
 *   - src/interpreter/JIT.hpp - registers symbols with JIT session
 *   - src/interpreter/Interpreter.hpp - uses DynLink for foreign libraries
 *   - src/compiler/IRLowering.hpp - emits declare statements for foreign functions
 * 
 * @par Platform Support
 *   - Windows: LoadLibraryA / GetProcAddress
 *   - Linux: dlopen / dlsym
 *   - macOS: dlopen / dlsym
 */

#pragma once

#include <windows.h>
#include <minwindef.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

// Forward declare JITSession to avoid circular dependency
namespace interpreter {
    class JITSession;
}

namespace interpreter {

/**
 * @brief Exception thrown when dynamic linking operations fail.
 */
class DynLinkError : public std::runtime_error {
public:
    enum class Kind {
        LibraryNotFound,     // Library file not found
        LibraryLoadFailed,   // Failed to load library
        SymbolNotFound,      // Symbol not found in library
        SymbolExtractFailed, // Failed to extract symbols
        AlreadyLoaded,       // Library already loaded
        NotLoaded,           // Library not loaded (for unload)
    };

    DynLinkError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

/**
 * @brief Platform-agnostic dynamic library loader.
 * 
 * Handles loading shared libraries, extracting symbols, and registering
 * them with the JIT's symbol table for foreign function resolution.
 * 
 * @par Usage Example
 * @code
 *   DynLink dynLink;
 *   
 *   // Load a library by name
 *   if (dynLink.load("opengl")) {
 *       // Register all symbols with the JIT
 *       dynLink.registerWithJIT(jit);
 *   }
 *   
 *   // Get a specific symbol
 *   auto* fn = dynLink.getSymbol("glClear");
 *   if (fn) {
 *       auto glClear = reinterpret_cast<void(*)(uint32_t)>(fn);
 *       glClear(0x00004000); // GL_COLOR_BUFFER_BIT
 *   }
 * @endcode
 * 
 * @par Platform Differences
 *   - Windows: Libraries are .dll files, loaded with LoadLibraryA
 *   - Linux: Libraries are .so files, loaded with dlopen
 *   - macOS: Libraries are .dylib files, loaded with dlopen
 */
class DynLink {
public:
    DynLink();
    ~DynLink();

    // Non-copyable
    DynLink(const DynLink&) = delete;
    DynLink& operator=(const DynLink&) = delete;

    // Moveable
    DynLink(DynLink&&) = default;
    DynLink& operator=(DynLink&&) = default;

    /**
     * @brief Load a shared library by name.
     * 
     * Searches the system library paths for the library.
     * On Windows, appends .dll; on Linux, prepends lib and appends .so.
     * 
     * @param libraryName The library name (e.g., "opengl", "m", "c")
     * @return true on success
     * @throws DynLinkError if the library cannot be loaded
     */
    bool load(const std::string& libraryName);

    /**
     * @brief Load a shared library from a specific path.
     * 
     * @param path Full path to the library file
     * @return true on success
     * @throws DynLinkError if the library cannot be loaded
     */
    bool loadPath(const std::string& path);

    /**
     * @brief Unload a previously loaded library.
     * 
     * @param libraryName The library name
     * @return true if the library was found and unloaded
     */
    bool unload(const std::string& libraryName);

    /**
     * @brief Get a symbol from a loaded library.
     * 
     * @param symbolName The symbol name (e.g., "glClear", "printf")
     * @return Pointer to the symbol, or nullptr if not found
     */
    void* getSymbol(const std::string& symbolName) const;

    /**
     * @brief Get all symbols from all loaded libraries.
     * 
     * @return Map of symbol name → symbol address
     */
    std::unordered_map<std::string, void*> getAllSymbols() const;

    /**
     * @brief Get symbols from a specific library.
     * 
     * @param libraryName The library name
     * @return Map of symbol name → symbol address, or empty if not found
     */
    std::unordered_map<std::string, void*> getLibrarySymbols(
        const std::string& libraryName) const;

    /**
     * @brief Register all loaded library symbols with the JIT.
     * 
     * This adds the library's symbols to the JIT's symbol table,
     * allowing JIT-compiled code to call foreign functions.
     * 
     * @param jit The JIT session to register with
     */
    void registerWithJIT(JITSession& jit);

    /**
     * @brief Register a specific library's symbols with the JIT.
     * 
     * @param jit The JIT session to register with
     * @param libraryName The library name
     * @return true if the library was found and registered
     */
    bool registerLibraryWithJIT(JITSession& jit, const std::string& libraryName);

    /**
     * @brief Check if a library is loaded.
     * 
     * @param libraryName The library name
     * @return true if the library is loaded
     */
    bool isLoaded(const std::string& libraryName) const;

    /**
     * @brief Get the list of loaded library names.
     * 
     * @return Vector of library names
     */
    std::vector<std::string> getLoadedLibraries() const;

private:
    /**
     * @brief Internal handle for a loaded library.
     */
    struct LibraryHandle {
        std::string name;                              // Library name (for lookup)
        std::string path;                              // Full path to the library
#ifdef _WIN32
        HMODULE handle;                                // Windows module handle
#else
        void* handle;                                  // POSIX dlopen handle
#endif
        std::unordered_map<std::string, void*> symbols; // Extracted symbols
        bool symbolsExtracted = false;
    };

    using LibraryHandlePtr = std::unique_ptr<LibraryHandle>;

    /**
     * @brief Extract all exported symbols from a loaded library.
     * 
     * @param lib The library handle to extract symbols from
     */
    void extractSymbols(LibraryHandle& lib);

    /**
     * @brief Get the platform-specific library filename.
     * 
     * @param name The library name (e.g., "opengl")
     * @return The platform-specific filename (e.g., "opengl.dll" on Windows)
     */
    std::string getLibraryFileName(const std::string& name) const;

    /**
     * @brief Get the platform-specific library path.
     * 
     * @param name The library name
     * @return The full library path
     */
    std::string getLibraryPath(const std::string& name) const;

    /**
     * @brief Convert a Windows error code to a string.
     * 
     * @param errorCode The Windows error code (GetLastError)
     * @return The error message string
     */
#ifdef _WIN32
    std::string getWindowsErrorString(DWORD errorCode) const;
#endif

    // ─── Members ─────────────────────────────────────────────────────────────

    std::unordered_map<std::string, LibraryHandlePtr> m_libraries;
    bool m_initialized = false;

    // Cache for fast symbol lookup across all libraries
    mutable std::unordered_map<std::string, void*> m_symbolCache;
    mutable bool m_cacheDirty = true;
};

} // namespace interpreter