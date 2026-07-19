/**
 * @file DynLink.cpp
 * @brief Implementation of the dynamic library loader.
 */

#include "DynLink.hpp"
#include "JIT.hpp"

// #include "llvm/ExecutionEngine/Orc/DynamicLibrarySearchGenerator.h"
#include "llvm/Support/DynamicLibrary.h"

#include <iostream>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <libloaderapi.h>
#include <errhandlingapi.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <limits.h>
#endif

namespace interpreter {

// ─────────────────────────────────────────────────────────────────────────────
// DynLink - Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

DynLink::DynLink() = default;

DynLink::~DynLink() {
    // Unload all libraries
    for (auto& pair : m_libraries) {
        auto& lib = pair.second;
#ifdef _WIN32
        if (lib->handle) {
            FreeLibrary(lib->handle);
        }
#else
        if (lib->handle) {
            dlclose(lib->handle);
        }
#endif
    }
    m_libraries.clear();
    m_symbolCache.clear();
    m_cacheDirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DynLink - Library Loading
// ─────────────────────────────────────────────────────────────────────────────

bool DynLink::load(const std::string& libraryName) {
    if (isLoaded(libraryName)) {
        throw DynLinkError(DynLinkError::Kind::AlreadyLoaded,
                           "Library already loaded: " + libraryName);
    }

    std::string libPath = getLibraryPath(libraryName);
    return loadPath(libPath);
}

bool DynLink::loadPath(const std::string& path) {
    // Create the library handle
    auto lib = std::make_unique<LibraryHandle>();
    lib->path = path;

    // Extract the library name from the path
    size_t lastSlash = path.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) 
        ? path.substr(lastSlash + 1) 
        : path;
    
    // Remove extension
    size_t lastDot = filename.find_last_of('.');
    lib->name = (lastDot != std::string::npos) 
        ? filename.substr(0, lastDot) 
        : filename;

#ifdef _WIN32
    // Windows: LoadLibraryA
    lib->handle = LoadLibraryA(path.c_str());
    if (!lib->handle) {
        DWORD error = GetLastError();
        throw DynLinkError(DynLinkError::Kind::LibraryLoadFailed,
                           "Failed to load library '" + path + "': " +
                           getWindowsErrorString(error));
    }
#else
    // POSIX: dlopen
    lib->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!lib->handle) {
        const char* error = dlerror();
        throw DynLinkError(DynLinkError::Kind::LibraryLoadFailed,
                           "Failed to load library '" + path + "': " +
                           (error ? error : "unknown error"));
    }
#endif

    // Extract symbols from the library
    try {
        extractSymbols(*lib);
    } catch (const DynLinkError& e) {
        // Clean up on extraction failure
#ifdef _WIN32
        FreeLibrary(lib->handle);
#else
        dlclose(lib->handle);
#endif
        throw;
    }

    // Store the library
    std::string libName = lib->name;
    m_libraries[libName] = std::move(lib);
    m_cacheDirty = true;

    return true;
}

bool DynLink::unload(const std::string& libraryName) {
    auto it = m_libraries.find(libraryName);
    if (it == m_libraries.end()) {
        return false;
    }

    auto& lib = it->second;
#ifdef _WIN32
    if (lib->handle) {
        FreeLibrary(lib->handle);
    }
#else
    if (lib->handle) {
        dlclose(lib->handle);
    }
#endif

    m_libraries.erase(it);
    m_cacheDirty = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DynLink - Symbol Resolution
// ─────────────────────────────────────────────────────────────────────────────

void DynLink::extractSymbols(LibraryHandle& lib) {
    if (lib.symbolsExtracted) {
        return;
    }

    lib.symbols.clear();

#ifdef _WIN32
    // Windows: Extract symbols using GetProcAddress
    // Note: Windows doesn't provide a way to enumerate all exported symbols
    // without parsing the PE header. Instead, we'll use GetProcAddress
    // for specific symbols as they're requested.
    // 
    // We'll mark the library as having symbols "extracted" but the actual
    // symbol lookup will be done lazily in getSymbol().
    lib.symbolsExtracted = true;
    return;
#else
    // POSIX: Extract symbols using dladdr and dlsym
    // For POSIX, we can't easily enumerate all symbols either.
    // We'll use lazy lookup in getSymbol().
    lib.symbolsExtracted = true;
    return;
#endif
}

void* DynLink::getSymbol(const std::string& symbolName) const {
    // Check cache first
    if (m_cacheDirty) {
        m_symbolCache.clear();
        m_cacheDirty = false;
        
        // Build cache from all libraries
        for (const auto& pair : m_libraries) {
            const auto& lib = pair.second;
            for (const auto& symPair : lib->symbols) {
                m_symbolCache[symPair.first] = symPair.second;
            }
        }
    }

    // Check cache
    auto cacheIt = m_symbolCache.find(symbolName);
    if (cacheIt != m_symbolCache.end()) {
        return cacheIt->second;
    }

    // Not in cache - try to find it in any library
    for (const auto& pair : m_libraries) {
        const auto& lib = pair.second;
        
#ifdef _WIN32
        // Windows: Use GetProcAddress
        void* symbol = reinterpret_cast<void*>(GetProcAddress(lib->handle, symbolName.c_str()));
        if (symbol) {
            // Cache it for future lookups
            m_symbolCache[symbolName] = symbol;
            return symbol;
        }
#else
        // POSIX: Use dlsym
        // Clear any existing error
        dlerror();
        void* symbol = dlsym(lib->handle, symbolName.c_str());
        const char* error = dlerror();
        if (symbol) {
            // Cache it for future lookups
            m_symbolCache[symbolName] = symbol;
            return symbol;
        }
#endif
    }

    return nullptr;
}

std::unordered_map<std::string, void*> DynLink::getAllSymbols() const {
    std::unordered_map<std::string, void*> allSymbols;
    
    for (const auto& pair : m_libraries) {
        const auto& lib = pair.second;
        for (const auto& symPair : lib->symbols) {
            allSymbols[symPair.first] = symPair.second;
        }
    }
    
    return allSymbols;
}

std::unordered_map<std::string, void*> DynLink::getLibrarySymbols(
    const std::string& libraryName) const {
    
    auto it = m_libraries.find(libraryName);
    if (it == m_libraries.end()) {
        return {};
    }
    
    return it->second->symbols;
}

// ─────────────────────────────────────────────────────────────────────────────
// DynLink - JIT Registration
// ─────────────────────────────────────────────────────────────────────────────

void DynLink::registerWithJIT(JITSession& jit) {
    for (const auto& pair : m_libraries) {
        registerLibraryWithJIT(jit, pair.first);
    }
}

bool DynLink::registerLibraryWithJIT(JITSession& jit, const std::string& libraryName) {
    auto it = m_libraries.find(libraryName);
    if (it == m_libraries.end()) {
        return false;
    }

    const auto& lib = it->second;

    // Create a DynamicLibrarySearchGenerator for the library
    auto dylib = llvm::orc::DynamicLibrarySearchGenerator::Load(
        lib->path.c_str()
    );

    if (!dylib) {
        throw DynLinkError(DynLinkError::Kind::SymbolExtractFailed,
                           "Failed to create search generator for library: " + libraryName);
    }

    // Add the generator to the JIT's main dylib
    auto error = jit.getJIT().getMainJITDylib().addGenerator(std::move(*dylib));
    if (error) {
        throw DynLinkError(DynLinkError::Kind::SymbolExtractFailed,
                           "Failed to register library symbols: " +
                           llvm::toString(std::move(error)));
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DynLink - Queries
// ─────────────────────────────────────────────────────────────────────────────

bool DynLink::isLoaded(const std::string& libraryName) const {
    return m_libraries.find(libraryName) != m_libraries.end();
}

std::vector<std::string> DynLink::getLoadedLibraries() const {
    std::vector<std::string> names;
    names.reserve(m_libraries.size());
    for (const auto& pair : m_libraries) {
        names.push_back(pair.first);
    }
    return names;
}

// ─────────────────────────────────────────────────────────────────────────────
// DynLink - Platform Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string DynLink::getLibraryFileName(const std::string& name) const {
#ifdef _WIN32
    return name + ".dll";
#elif __APPLE__
    return "lib" + name + ".dylib";
#else
    return "lib" + name + ".so";
#endif
}

std::string DynLink::getLibraryPath(const std::string& name) const {
    // Check if the name already has a path separator or extension
    bool hasPath = name.find('/') != std::string::npos ||
                   name.find('\\') != std::string::npos;
    bool hasExtension = name.find('.') != std::string::npos;

    if (hasPath && hasExtension) {
        // It's already a full path
        return name;
    } else if (hasPath) {
        // Has path but no extension - add the platform extension
        return name + "." + (
#ifdef _WIN32
            "dll"
#elif __APPLE__
            "dylib"
#else
            "so"
#endif
        );
    } else {
        // Just a library name - use the platform naming convention
        return getLibraryFileName(name);
    }
}

#ifdef _WIN32
std::string DynLink::getWindowsErrorString(DWORD errorCode) const {
    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr
    );

    std::string message;
    if (size > 0 && messageBuffer) {
        message = messageBuffer;
        // Remove trailing newline characters
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
        LocalFree(messageBuffer);
    } else {
        message = "Unknown error (code: " + std::to_string(errorCode) + ")";
    }

    return message;
}
#endif

} // namespace interpreter