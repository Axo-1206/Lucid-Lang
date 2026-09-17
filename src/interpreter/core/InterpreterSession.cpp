/// @file core/InterpreterSession.cpp
/// @brief Session-level state implementation.

#include "InterpreterSession.hpp"
#include "../support/InterpreterError.hpp"

#include <iostream>

namespace interpreter {

InterpreterSession::InterpreterSession(StringPool& pool,
                                       DiagnosticEngine& diag,
                                       const InterpreterOptions& options)
    : m_pool(pool)
    , m_diag(diag)
    , m_options(options)
    , m_jit(pool) {
    m_instanceTable.assign(kDefaultModuleCapacity, nullptr);
}

InterpreterSession::~InterpreterSession() {
    shutdown();
}

void InterpreterSession::initialize() {
    if (m_initialized) {
        return;
    }

    try {
        m_jit.initialize();

        // Reset and register instance table as absolute symbol
        m_instanceTable.assign(kDefaultModuleCapacity, nullptr);
        m_jit.defineAbsoluteSymbol("__lucid_module_instances", m_instanceTable.data());

        m_initialized = true;

        if (m_options.verbose) {
            std::cout << "Interpreter session initialized successfully\n";
            std::cout << "  Module capacity: " << kDefaultModuleCapacity << "\n";
            std::cout << "  Optimization level: " << m_options.optimizationLevel << "\n";
            std::cout << "  Debug info: " << (m_options.enableDebugInfo ? "enabled" : "disabled") << "\n";
            std::cout << "  Hot-reload: " << (m_options.enableHotReload ? "enabled" : "disabled") << "\n";
        }
    } catch (const std::exception& e) {
        m_diag.error(DiagCode::Backend_CodegenError, nullptr,
                     "interpreter initialization failed: ", e.what());
        throw InterpreterError(InterpreterErrorKind::InitFailed, e.what());
    }
}

void InterpreterSession::shutdown() {
    if (!m_initialized) {
        return;
    }

    // Instance table slots are cleared
    std::fill(m_instanceTable.begin(), m_instanceTable.end(), nullptr);
    m_initialized = false;
}

void InterpreterSession::registerLibrarySymbols(const std::string& path, const std::string& name) {
    m_jit.registerLibrarySymbols(path, name);
}

} // namespace interpreter
