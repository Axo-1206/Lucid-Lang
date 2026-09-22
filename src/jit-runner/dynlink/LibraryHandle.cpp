/// @file dynlink/LibraryHandle.cpp
/// @brief RAII wrapper for dynamic library handles.

#include "LibraryHandle.hpp"
#include "../support/InterpreterError.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <cstring>
#endif

namespace interpreter {

// ─── LibraryHandle ──────────────────────────────────────────────────────

LibraryHandle::LibraryHandle(const std::string& path)
    : m_path(path)
    , m_handle(nullptr) {
#ifdef _WIN32
    m_handle = LoadLibraryA(path.c_str());
    if (!m_handle) {
        throw InterpreterError(InterpreterErrorKind::LibraryLoadFailed,
                              "Failed to load library '" + path + "': " + 
                              std::to_string(GetLastError()));
    }
#else
    m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m_handle) {
        const char* error = dlerror();
        throw InterpreterError(InterpreterErrorKind::LibraryLoadFailed,
                              "Failed to load library '" + path + "': " + 
                              (error ? error : "unknown error"));
    }
#endif
}

LibraryHandle::~LibraryHandle() {
    unload();
}

LibraryHandle::LibraryHandle(LibraryHandle&& other) noexcept
    : m_handle(other.m_handle)
    , m_path(std::move(other.m_path)) {
    other.m_handle = nullptr;
}

LibraryHandle& LibraryHandle::operator=(LibraryHandle&& other) noexcept {
    if (this != &other) {
        unload();
        m_handle = other.m_handle;
        m_path = std::move(other.m_path);
        other.m_handle = nullptr;
    }
    return *this;
}

void* LibraryHandle::getSymbol(const std::string& name) const {
    if (!m_handle) {
        return nullptr;
    }

#ifdef _WIN32
    // GetProcAddress returns FARPROC (function pointer type)
    // We need to cast it to void* for the interface
    FARPROC symbol = GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name.c_str());
    if (!symbol) {
        return nullptr;
    }
    // Cast FARPROC to void* - this is necessary for the API
    // Note: This is a Microsoft extension, but it's the only way to handle
    // function pointers uniformly across platforms.
    return reinterpret_cast<void*>(symbol);
#else
    // Clear any existing error
    dlerror();
    void* symbol = dlsym(m_handle, name.c_str());
    const char* error = dlerror();
    if (error) {
        return nullptr;
    }
    return symbol;
#endif
}

std::string LibraryHandle::getName() const {
    if (m_path.empty()) {
        return "";
    }

    // Extract filename from path
    size_t lastSlash = m_path.find_last_of("/\\");
    std::string name = (lastSlash != std::string::npos) 
        ? m_path.substr(lastSlash + 1) 
        : m_path;

    // Remove extension
    size_t lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos) {
        name = name.substr(0, lastDot);
    }

    // On Windows, remove leading "lib" if present (on Unix, lib is part of the name)
#ifdef _WIN32
    // Windows DLLs don't typically have lib prefix
#else
    // On Unix, dynamic libraries often have lib prefix
    if (name.compare(0, 3, "lib") == 0) {
        name = name.substr(3);
    }
#endif

    return name;
}

void LibraryHandle::unload() {
    if (m_handle) {
#ifdef _WIN32
        FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
        dlclose(m_handle);
#endif
        m_handle = nullptr;
    }
}

} // namespace interpreter