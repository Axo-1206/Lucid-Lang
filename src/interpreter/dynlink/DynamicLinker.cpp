/// @file dynlink/DynamicLinker.cpp
/// @brief Platform-agnostic dynamic library loader.

#include "DynamicLinker.hpp"
#include "../jit/JITSession.hpp"
#include "../support/InterpreterError.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"

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

// ─── Library Loading ──────────────────────────────────────────────────

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

void DynamicLinker::registerLibrariesFromModule(DiagnosticEngine& diagnostics,
                                                StringPool& pool,
                                                bool verbose,
                                                ModuleAST* module) {
    if (!module) return;

    InternedString linkName = pool.intern("link");
    for (DeclAST* decl : module->decls) {
        for (AttributePtr attr : decl->attributes) {
            if (attr->name == linkName) {
                for (LiteralExprAST* arg : attr->args) {
                    if (arg->kind == LiteralKind::String || 
                        arg->kind == LiteralKind::RawString) {
                        std::string libName = pool.lookup(arg->value);
                        bool isPath = libName.find('/') != std::string::npos ||
                                      libName.find('\\') != std::string::npos ||
                                      libName.find('.') != std::string::npos;
                        if (!isPath) {
                            try {
                                load(libName);
                            } catch (const InterpreterError& e) {
                                if (verbose) {
                                    std::cerr << "Warning: " << e.what() << "\n";
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void DynamicLinker::registerLibrariesFromModules(DiagnosticEngine& diagnostics,
                                                 StringPool& pool,
                                                 bool verbose,
                                                 const std::vector<ModuleAST*>& modules) {
    for (ModuleAST* module : modules) {
        registerLibrariesFromModule(diagnostics, pool, verbose, module);
    }
}

// ─── Symbol Lookup ──────────────────────────────────────────────────────

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
    return nullptr;  // Needs StringPool access
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

    return symbols;
}

// ─── JIT Integration ──────────────────────────────────────────────────

void DynamicLinker::registerWithJIT(JITSession& jit) {
    for (const auto& [name, handle] : m_libraries) {
        if (handle && handle->isLoaded()) {
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

// ─── Query ─────────────────────────────────────────────────────────────

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

// ─── Private Helpers ──────────────────────────────────────────────────

void DynamicLinker::rebuildCache() const {
    m_symbolCache.clear();
    m_cacheDirty = false;
}

} // namespace interpreter