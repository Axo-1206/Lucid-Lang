/// @file jit/JITCompiler.cpp
/// @brief Compiles LLVM IR modules to executable code.

#include "JITCompiler.hpp"

#include "../support/InterpreterError.hpp"

#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

namespace interpreter {

JITCompiler::JITCompiler(JITSession& session)
    : m_session(session) {
}

void JITCompiler::compile(std::unique_ptr<llvm::Module> module, InternedString name) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                              "Cannot compile null module");
    }

    if (!name.isValid()) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                              "Cannot compile module with invalid name");
    }

    try {
        // Ensure the JIT is initialized
        if (!m_session.isInitialized()) {
            m_session.initialize();
        }

        // Remove any existing module with this name
        if (m_session.hasModule(name)) {
            m_session.removeModule(name);
        }

        // Add the module to the JIT
        m_session.addModule(std::move(module), name);
    } catch (const JITError& e) {
        throw InterpreterError(InterpreterErrorKind::JITError,
                              std::string("Compilation failed: ") + e.what());
    } catch (const std::exception& e) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                              std::string("Compilation failed: ") + e.what());
    }
}

bool JITCompiler::remove(InternedString name) {
    if (!name.isValid()) {
        return false;
    }

    try {
        if (!m_session.isInitialized()) {
            return false;
        }

        return m_session.removeModule(name);
    } catch (const JITError& e) {
        // Log the error but return false
        std::cerr << "Failed to remove module: " << e.what() << "\n";
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Failed to remove module: " << e.what() << "\n";
        return false;
    }
}

bool JITCompiler::isLoaded(InternedString name) const {
    if (!name.isValid() || !m_session.isInitialized()) {
        return false;
    }
    return m_session.hasModule(name);
}

void* JITCompiler::lookup(InternedString name) {
    if (!name.isValid() || !m_session.isInitialized()) {
        return nullptr;
    }

    try {
        return m_session.lookupSymbol(name);
    } catch (const JITError& e) {
        // Symbol not found - return nullptr
        return nullptr;
    }
}

void* JITCompiler::lookup(const std::string& name) {
    if (name.empty() || !m_session.isInitialized()) {
        return nullptr;
    }

    try {
        return m_session.lookupSymbol(name);
    } catch (const JITError& e) {
        // Symbol not found - return nullptr
        return nullptr;
    }
}

} // namespace interpreter