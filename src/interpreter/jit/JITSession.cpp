/// @file jit/JITSession.cpp
/// @brief ORC JIT session management implementation.

#include "JITSession.hpp"

#include "../support/InterpreterError.hpp"

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/TargetParser/Host.h"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace interpreter {

// ─── JITError ───────────────────────────────────────────────────────────

// JITError is defined in the header, no additional implementation needed.

// ─── JITSession ─────────────────────────────────────────────────────────

JITSession::JITSession(StringPool& stringPool)
    : m_stringPool(stringPool)
    , m_context(std::make_unique<llvm::LLVMContext>()) {
}

JITSession::~JITSession() {
    // LLJIT will clean up resources automatically
}

void JITSession::initialize() {
    if (m_initialized) {
        return;
    }

    try {
        // Initialize LLVM targets
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        // Set up the JIT
        setupTarget();
        setupPlatformLibraries();

        m_initialized = true;
    } catch (const std::exception& e) {
        throw JITError(JITError::Kind::InitFailed, 
                      std::string("JIT initialization failed: ") + e.what());
    }
}

void JITSession::setupTarget() {
    // Get the host target triple
    auto JTMB = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!JTMB) {
        llvm::handleAllErrors(JTMB.takeError(), [](const llvm::ErrorInfoBase& EI) {
            throw JITError(JITError::Kind::InitFailed, 
                          std::string("Failed to detect host target: ") + EI.message());
        });
    }

    // Create the JIT
    auto JIT = llvm::orc::LLJITBuilder()
        .setJITTargetMachineBuilder(std::move(*JTMB))
        .create();
    
    if (!JIT) {
        llvm::handleAllErrors(JIT.takeError(), [](const llvm::ErrorInfoBase& EI) {
            throw JITError(JITError::Kind::InitFailed, 
                          std::string("Failed to create JIT: ") + EI.message());
        });
    }

    m_jit = std::move(*JIT);
}

void JITSession::setupPlatformLibraries() {
    // Load platform-specific libraries
#ifdef _WIN32
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
#else
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
#endif

    // Get the generator for the current process
    auto Generator = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        m_jit->getDataLayout().getGlobalPrefix());

    // Unwrap the Expected and directly add the unique_ptr to the JITDylib
    m_jit->getMainJITDylib().addGenerator(
        llvm::cantFail(std::move(Generator))
    );
}

void JITSession::registerLibrarySymbols(const std::string& path, const std::string& name) {
    // Register symbols from a dynamic library with the JIT
    // Load the library and add its symbols to the JIT's search path
    
    // First, try to load the library using the system dynamic loader
    // This makes the symbols available to the JIT's symbol resolver
    
#ifdef _WIN32
    // On Windows, use LoadLibrary to load the DLL
    HMODULE handle = LoadLibraryA(path.c_str());
    if (handle) {
        // The DLL is now loaded in the process. The JIT will find symbols
        // through the process's symbol table.
        // We don't need to keep the handle - the DLL will stay loaded.
    } else {
        std::cerr << "Warning: Failed to load library for JIT: " << path 
                  << " (error: " << GetLastError() << ")\n";
    }
#else
    // On Unix, use dlopen with RTLD_GLOBAL to make symbols available
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle) {
        // The library is now loaded and its symbols are global.
        // The JIT will find them through the process's symbol table.
    } else {
        const char* error = dlerror();
        std::cerr << "Warning: Failed to load library for JIT: " << path 
                  << " (error: " << (error ? error : "unknown") << ")\n";
    }
#endif

    // Note: The JIT already has the current process's symbols via
    // DynamicLibrarySearchGenerator::GetForCurrentProcess() in setupPlatformLibraries().
    // Any library we load with the system loader will have its symbols
    // automatically available to the JIT.
    (void)path;
    (void)name;
}

void JITSession::addModule(std::unique_ptr<llvm::Module> module, InternedString name) {
    if (!m_initialized) {
        throw JITError(JITError::Kind::ModuleAddFailed, "JIT not initialized");
    }

    if (!module) {
        throw JITError(JITError::Kind::ModuleAddFailed, "Cannot add null module");
    }

    // Verify the module
    std::string errStr;
    llvm::raw_string_ostream errStream(errStr);
    if (llvm::verifyModule(*module, &errStream)) {
        throw JITError(JITError::Kind::ModuleAddFailed,
                      std::string("Module verification failed: ") + errStr);
    }

    // Set the module's target triple and data layout to match the JIT
    module->setTargetTriple(m_jit->getTargetTriple().str());
    module->setDataLayout(m_jit->getDataLayout());

    // Create a ThreadSafeModule with its own context
    auto moduleContext = std::make_unique<llvm::LLVMContext>();
    auto threadSafeModule = llvm::orc::ThreadSafeModule(std::move(module), std::move(moduleContext));

    // Add the module to the JIT
    auto tracker = m_jit->getMainJITDylib().createResourceTracker();
    
    if (auto Err = m_jit->addIRModule(tracker, std::move(threadSafeModule))) {
        llvm::handleAllErrors(std::move(Err), [&](const llvm::ErrorInfoBase& EI) {
            throw JITError(JITError::Kind::ModuleAddFailed,
                          std::string("Failed to add module: ") + EI.message());
        });
    }

    // Store the tracker for later removal
    m_trackers[name.id] = tracker;
}

bool JITSession::removeModule(InternedString name) {
    if (!m_initialized) {
        throw JITError(JITError::Kind::ModuleRemoveFailed, "JIT not initialized");
    }

    auto it = m_trackers.find(name.id);
    if (it == m_trackers.end()) {
        return false;
    }

    // Remove the module
    if (auto Err = it->second->remove()) {
        llvm::handleAllErrors(std::move(Err), [&](const llvm::ErrorInfoBase& EI) {
            throw JITError(JITError::Kind::ModuleRemoveFailed,
                          std::string("Failed to remove module: ") + EI.message());
        });
    }

    m_trackers.erase(it);
    return true;
}

bool JITSession::hasModule(InternedString name) const {
    return m_trackers.find(name.id) != m_trackers.end();
}

void* JITSession::lookupSymbol(const std::string& name) {
    if (!m_initialized) {
        return nullptr;
    }

    // Look up the symbol in the JIT
    auto symbol = m_jit->lookup(name);
    if (!symbol) {
        // Symbol not found
        return nullptr;
    }

    // Convert to a function pointer
    return reinterpret_cast<void*>((*symbol).getValue());
}

void* JITSession::lookupSymbol(InternedString name) {
    if (!name.isValid()) {
        return nullptr;
    }
    return lookupSymbol(internedToString(name));
}

std::string JITSession::internedToString(InternedString name) const {
    return m_stringPool.lookup(name);
}

} // namespace interpreter