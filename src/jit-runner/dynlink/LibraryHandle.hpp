/// @file dynlink/LibraryHandle.hpp
/// @brief RAII wrapper for dynamic library handles.

#pragma once

#include <string>
#include <memory>

namespace interpreter {

/// @brief RAII wrapper for a dynamic library handle.
///
/// Automatically loads the library on construction and unloads it on destruction.
/// Provides platform-agnostic symbol lookup.
class LibraryHandle {
public:
    /// @brief Load a library from the given path.
    /// @throws std::runtime_error if the library cannot be loaded.
    explicit LibraryHandle(const std::string& path);

    /// @brief Destructor - automatically unloads the library.
    ~LibraryHandle();

    // Non-copyable
    LibraryHandle(const LibraryHandle&) = delete;
    LibraryHandle& operator=(const LibraryHandle&) = delete;

    // Moveable
    LibraryHandle(LibraryHandle&& other) noexcept;
    LibraryHandle& operator=(LibraryHandle&& other) noexcept;

    /// @brief Get a symbol from the library.
    /// @param name The symbol name.
    /// @return Pointer to the symbol, or nullptr if not found.
    void* getSymbol(const std::string& name) const;

    /// @brief Check if the library is loaded.
    bool isLoaded() const { return m_handle != nullptr; }

    /// @brief Get the library path.
    const std::string& getPath() const { return m_path; }

    /// @brief Get the library name (filename without extension).
    std::string getName() const;

private:
    void* m_handle = nullptr;
    std::string m_path;

#ifdef _WIN32
    using HandleType = void*;  // HMODULE is void*
#else
    using HandleType = void*;  // void* from dlopen
#endif

    void unload();
};

} // namespace interpreter