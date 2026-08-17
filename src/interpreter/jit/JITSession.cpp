/// @file jit/JITSession.cpp
/// @brief ORC JIT session management implementation.

#include "JITSession.hpp"

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
    // Load platform-specific libraries that may be needed for runtime symbols.
    // This is primarily for things like malloc, free, printf, etc.
    
#ifdef _WIN32
    // On Windows, we need to load the CRT
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
#else
    // On Unix-like systems, load the dynamic linker
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
#endif

    // Add the current process's symbols to the JIT's search path
    if (auto Err = m_jit->getMainJITDylib().addGenerator(
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                m_jit->getDataLayout().getGlobalPrefix()))) {
        llvm::handleAllErrors(std::move(Err), [](const llvm::ErrorInfoBase& EI) {
            std::cerr << "Warning: Failed to add current process symbols: " 
                      << EI.message() << "\n";
        });
    }
}

void JITSession::registerLibrarySymbols(const std::string& path, const std::string& name) {
    // Register symbols from a dynamic library with the JIT
    // This is a stub - actual implementation would use dlopen/LoadLibrary
    // and register the symbols with the JIT's symbol table
    
    // For now, we rely on the current process's symbols being available
    (void)path;
    (void)name;
    
    // TODO: Implement proper dynamic library loading and symbol registration
    // using llvm::orc::DynamicLibrarySearchGenerator
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

    // Create a ThreadSafeModule
    auto threadSafeModule = llvm::orc::ThreadSafeModule(std::move(module), m_context);

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
    return reinterpret_cast<void*>(symbol->getAddress());
}

void* JITSession::lookupSymbol(InternedString name) {
    if (!name.isValid()) {
        return nullptr;
    }
    return lookupSymbol(internedToString(name));
}

std::string JITSession::internedToString(InternedString name) const {
    // Use the string pool to convert InternedString to std::string
    // The StringPool class would need a lookup method
    // For now, we assume a method exists
    return m_stringPool.lookup(name);
}

} // namespace interpreter