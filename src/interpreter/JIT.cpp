/**
 * @file JIT.cpp
 * @brief Implementation of the JIT session manager.
 */

#include "JIT.hpp"

#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <libloaderapi.h>
#else
#include <dlfcn.h>
#endif

namespace interpreter {

// ─────────────────────────────────────────────────────────────────────────────
// JITSession - Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

JITSession::JITSession()
    : m_context(std::make_unique<llvm::LLVMContext>()) {
}

JITSession::~JITSession() {
    // Resource trackers will be cleaned up by LLJIT destruction
}

// ─────────────────────────────────────────────────────────────────────────────
// JITSession - Error Handling (Define BEFORE any usage)
// ─────────────────────────────────────────────────────────────────────────────

template <>
void JITSession::handleError<void>(llvm::Error error, 
                                   JITError::Kind errorKind,
                                   const std::string& message) {
    if (!error) {
        return;
    }
    
    std::string errorMsg = llvm::toString(std::move(error));
    if (!message.empty()) {
        errorMsg = message + ": " + errorMsg;
    }
    
    throw JITError(errorKind, errorMsg);
}

template <typename T>
T JITSession::handleError(llvm::Error error, 
                          JITError::Kind errorKind,
                          const std::string& message) {
    if (!error) {
        return T{};
    }
    
    std::string errorMsg = llvm::toString(std::move(error));
    if (!message.empty()) {
        errorMsg = message + ": " + errorMsg;
    }
    
    throw JITError(errorKind, errorMsg);
}

// ─────────────────────────────────────────────────────────────────────────────
// JITSession - Initialization
// ─────────────────────────────────────────────────────────────────────────────

bool JITSession::initialize() {
    if (m_initialized) {
        return true;
    }

    try {
        // 1. Setup target
        setupTarget();

        // 2. Create JIT instance
        auto jitBuilder = llvm::orc::LLJITBuilder();
        auto jitOrError = jitBuilder.create();
        if (auto error = jitOrError.takeError()) {
            handleError(std::move(error), JITError::Kind::InitFailed,
                        "Failed to create LLJIT instance");
            return false;
        }
        m_jit = std::move(*jitOrError);

        // 3. Register platform libraries
        setupPlatformLibraries();

        m_initialized = true;
        return true;

    } catch (const JITError& e) {
        // Re-throw JIT errors
        throw;
    } catch (const std::exception& e) {
        // Wrap unknown exceptions
        throw JITError(JITError::Kind::InitFailed,
                       "Unexpected initialization error: " + std::string(e.what()));
    }
}

void JITSession::setupTarget() {
    // Initialize LLVM target for the host
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    // Verify we can get the host triple
    auto triple = llvm::sys::getDefaultTargetTriple();
    if (triple.empty()) {
        throw JITError(JITError::Kind::InitFailed,
                       "Failed to get host target triple");
    }
}

void JITSession::setupPlatformLibraries() {
    // Load the kernel library - this provides all lge_* functions
    std::string kernelLibPath;

#ifdef _WIN32
    kernelLibPath = "luc_kernel.dll";
#elif __APPLE__
    kernelLibPath = "libluc_kernel.dylib";
#else
    kernelLibPath = "libluc_kernel.so";
#endif

    // Try to load the kernel library
    try {
        registerLibrarySymbols(kernelLibPath, "luc_kernel");
    } catch (const std::exception& e) {
        // Log but don't fail - kernel might be statically linked or loaded later
        std::cerr << "Warning: Failed to load " << kernelLibPath << ": " 
                  << e.what() << "\n";
        std::cerr << "Foreign symbols may not be available.\n";
    }

    // Open the current process for symbol lookup
    // This allows the JIT to find symbols from the host process
    // (including statically linked functions)
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
}

void JITSession::registerLibrarySymbols(const std::string& libraryPath,
                                        const std::string& libraryName) {
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(libraryPath.c_str());
    if (!handle) {
        DWORD error = GetLastError();
        std::stringstream ss;
        ss << "Failed to load library '" << libraryPath 
           << "' (error code: " << error << ")";
        throw JITError(JITError::Kind::InitFailed, ss.str());
    }

    // FIX: LoadLibraryPermanently with nullptr adds the host process
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
    
    // FIX: Use the single-argument version of Load
    auto dylib = llvm::orc::DynamicLibrarySearchGenerator::Load(
        libraryPath.c_str()
    );
    
    if (!dylib) {
        throw JITError(JITError::Kind::InitFailed,
                       "Failed to create search generator for: " + libraryPath);
    }
    
    if (auto error = m_jit->getMainJITDylib().addGenerator(std::move(*dylib))) {
        throw JITError(JITError::Kind::InitFailed,
                       "Failed to register library symbols: " + 
                       llvm::toString(std::move(error)));
    }

#else // POSIX (Linux, macOS, etc.)
    void* handle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char* error = dlerror();
        throw JITError(JITError::Kind::InitFailed,
                       "Failed to load library '" + libraryPath + 
                       "': " + (error ? error : "unknown error"));
    }

    // FIX: Same fix for POSIX
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
    
    // FIX: Use the single-argument version of Load
    auto dylib = llvm::orc::DynamicLibrarySearchGenerator::Load(
        libraryPath.c_str()
    );
    
    if (!dylib) {
        throw JITError(JITError::Kind::InitFailed,
                       "Failed to create search generator for: " + libraryPath);
    }
    
    if (auto error = m_jit->getMainJITDylib().addGenerator(std::move(*dylib))) {
        throw JITError(JITError::Kind::InitFailed,
                       "Failed to register library symbols: " + 
                       llvm::toString(std::move(error)));
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// JITSession - Module Management
// ─────────────────────────────────────────────────────────────────────────────

bool JITSession::addModule(std::unique_ptr<llvm::Module> module,
                           const std::string& moduleName) {
    if (!m_initialized) {
        throw JITError(JITError::Kind::ModuleAddFailed,
                       "JIT not initialized");
    }

    if (!module) {
        throw JITError(JITError::Kind::ModuleAddFailed,
                       "Cannot add null module");
    }

    // Verify the module is valid
    std::string errorMsg;
    llvm::raw_string_ostream errorStream(errorMsg);
    if (llvm::verifyModule(*module, &errorStream)) {
        throw JITError(JITError::Kind::ModuleAddFailed,
                       "Invalid IR module: " + errorMsg);
    }

    // Create a ThreadSafeModule (takes ownership)
    auto threadSafeModule = llvm::orc::ThreadSafeModule(
        std::move(module), std::move(m_context)
    );

    // Add the module to the JIT
    auto tracker = m_jit->getMainJITDylib().createResourceTracker();
    auto error = m_jit->addIRModule(tracker, std::move(threadSafeModule));
    
    if (error) {
        throw JITError(JITError::Kind::ModuleAddFailed,
                       "Failed to add module '" + moduleName + "': " +
                       llvm::toString(std::move(error)));
    }

    // Store the tracker for later removal
    m_trackers[moduleName] = std::move(tracker);

    return true;
}

bool JITSession::removeModule(const std::string& moduleName) {
    if (!m_initialized) {
        return false;
    }

    auto it = m_trackers.find(moduleName);
    if (it == m_trackers.end()) {
        return false; // Module not found, not an error
    }

    // Remove the module using its resource tracker
    auto error = it->second->remove();
    if (error) {
        throw JITError(JITError::Kind::ModuleRemoveFailed,
                       "Failed to remove module '" + moduleName + "': " +
                       llvm::toString(std::move(error)));
    }

    // Remove the tracker from our map
    m_trackers.erase(it);

    return true;
}

bool JITSession::hasModule(const std::string& moduleName) const {
    return m_trackers.find(moduleName) != m_trackers.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// JITSession - Symbol Lookup
// ─────────────────────────────────────────────────────────────────────────────

void* JITSession::lookupSymbol(const std::string& symbolName) {
    if (!m_initialized) {
        throw JITError(JITError::Kind::LookupFailed,
                       "JIT not initialized");
    }

    // Look up the symbol in the JIT
    auto symbolOrError = m_jit->lookup(symbolName);
    if (auto error = symbolOrError.takeError()) {
        std::string errMsg = llvm::toString(std::move(error));
        
        // FIX: Check for "symbol not found" by message content
        if (errMsg.find("symbol not found") != std::string::npos ||
            errMsg.find("Symbol not found") != std::string::npos) {
            return nullptr;
        }
        
        // Other errors are fatal
        throw JITError(JITError::Kind::LookupFailed,
                       "Failed to lookup symbol '" + symbolName + "': " + errMsg);
    }

    // FIX: Use getValue() instead of getAddress()
    auto symbol = symbolOrError.get();
    return reinterpret_cast<void*>(symbol.getValue());
}

// ─────────────────────────────────────────────────────────────────────────────
// Convenience helper for logging LLVM errors
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

std::string formatLLVMError(const llvm::Error& error) {
    std::string str;
    llvm::raw_string_ostream os(str);
    os << error;
    return str;
}

} // namespace detail

} // namespace interpreter