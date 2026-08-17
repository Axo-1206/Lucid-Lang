/// @file dynlink/DynamicLinker.cpp
/// @brief Platform-agnostic dynamic library loader.

#include "DynamicLinker.hpp"
#include "../jit/JITSession.hpp"
#include "../support/InterpreterError.hpp"

#include <algorithm>
#include <iostream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <libloaderapi.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace interpreter {

// ─── Platform-Specific Helpers ─────────────────────────────────────────

std::string DynamicLinker::getLibraryFileName(const std::string& name) const {
#ifdef _WIN32
    return name + ".dll";
#elif defined(__APPLE__)
    return "lib" + name + ".dylib";
#else
    return "lib" + name + ".so";
#endif
}

std::string DynamicLinker::getLibraryPath(const std::string& name) const {
    // First, check if it's already a path
    if (name.find('/') != std::string::npos || 
        name.find('\\') != std::string::npos ||
        name.find('.') != std::string::npos) {
        return name;
    }

    // Otherwise, construct the library filename
    return getLibraryFileName(name);
}

// ─── DynamicLinker ──────────────────────────────────────────────────────

bool DynamicLinker::load(const std::string& name) {
    if (isLoaded(name)) {
        return true;
    }

    std::string path = getLibraryPath(name);
    
    try {
        auto handle = std::make_unique<LibraryHandle>(path);
        m_libraries[name] = std::move(handle);
        m_cacheDirty = true;
        return true;
    } catch (const InterpreterError& e) {
        // Re-throw with more context
        throw InterpreterError(InterpreterErrorKind::LibraryLoadFailed,
                              "Failed to load library '" + name + "': " + e.what());
    }
}

bool DynamicLinker::loadPath(const std::string& path) {
    // Extract the base name from the path
    size_t lastSlash = path.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) 
        ? path.substr(lastSlash + 1) 
        : path;

    // Remove extension to get the name
    size_t lastDot = filename.find_last_of('.');
    std::string name = (lastDot != std::string::npos) 
        ? filename.substr(0, lastDot) 
        : filename;

    // On Unix, remove "lib" prefix if present
#ifndef _WIN32
    if (name.compare(0, 3, "lib") == 0) {
        name = name.substr(3);
    }
#endif

    if (isLoaded(name)) {
        return true;
    }

    try {
        auto handle = std::make_unique<LibraryHandle>(path);
        m_libraries[name] = std::move(handle);
        m_cacheDirty = true;
        return true;
    } catch (const InterpreterError& e) {
        // Re-throw with more context
        throw InterpreterError(InterpreterErrorKind::LibraryLoadFailed,
                              "Failed to load library from path '" + path + "': " + e.what());
    }
}

bool DynamicLinker::unload(const std::string& name) {
    auto it = m_libraries.find(name);
    if (it == m_libraries.end()) {
        return false;
    }

    m_libraries.erase(it);
    m_cacheDirty = true;
    return true;
}

void* DynamicLinker::getSymbol(const std::string& name) const {
    // Check cache first
    if (m_cacheDirty) {
        rebuildCache();
    }

    auto it = m_symbolCache.find(name);
    if (it != m_symbolCache.end()) {
        return it->second;
    }

    // Search all libraries
    for (const auto& [libName, handle] : m_libraries) {
        if (handle && handle->isLoaded()) {
            void* symbol = handle->getSymbol(name);
            if (symbol) {
                m_symbolCache[name] = symbol;
                return symbol;
            }
        }
    }

    return nullptr;
}

void* DynamicLinker::getSymbol(InternedString name) const {
    // Note: This needs a StringPool to convert. 
    // For now, we assume the caller handles the conversion.
    // This is a placeholder - the actual implementation would need
    // access to a StringPool.
    return nullptr;
}

std::unordered_map<std::string, void*> DynamicLinker::getAllSymbols() const {
    if (m_cacheDirty) {
        rebuildCache();
    }
    return m_symbolCache;
}

std::unordered_map<std::string, void*> DynamicLinker::getLibrarySymbols(
    const std::string& name) const {
    
    std::unordered_map<std::string, void*> symbols;

    auto it = m_libraries.find(name);
    if (it == m_libraries.end() || !it->second || !it->second->isLoaded()) {
        return symbols;
    }

    // Note: We cannot enumerate all symbols in a dynamic library
    // without platform-specific code. This is a limitation.
    // The caller should use getSymbol for specific symbols they need.
    
    return symbols;
}

void DynamicLinker::registerWithJIT(JITSession& jit) {
    for (const auto& [name, handle] : m_libraries) {
        if (handle && handle->isLoaded()) {
            // Register this library's symbols with the JIT
            // The JITSession will handle the actual registration
            jit.registerLibrarySymbols(handle->getPath(), name);
        }
    }
}

bool DynamicLinker::registerLibraryWithJIT(JITSession& jit, const std::string& name) {
    auto it = m_libraries.find(name);
    if (it == m_libraries.end() || !it->second || !it->second->isLoaded()) {
        return false;
    }

    jit.registerLibrarySymbols(it->second->getPath(), name);
    return true;
}

bool DynamicLinker::isLoaded(const std::string& name) const {
    auto it = m_libraries.find(name);
    if (it == m_libraries.end()) {
        return false;
    }
    return it->second && it->second->isLoaded();
}

std::vector<std::string> DynamicLinker::getLoadedLibraries() const {
    std::vector<std::string> result;
    result.reserve(m_libraries.size());

    for (const auto& [name, handle] : m_libraries) {
        if (handle && handle->isLoaded()) {
            result.push_back(name);
        }
    }

    return result;
}

void DynamicLinker::rebuildCache() const {
    m_symbolCache.clear();

    for (const auto& [name, handle] : m_libraries) {
        if (!handle || !handle->isLoaded()) {
            continue;
        }

        // We cannot enumerate all symbols, so we only cache symbols
        // that have been explicitly looked up.
        // This is a limitation of the platform APIs.
    }

    m_cacheDirty = false;
}

} // namespace interpreter