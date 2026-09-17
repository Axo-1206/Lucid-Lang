/// @file core/InterpreterSession.cpp
/// @brief Session-level state implementation.

#include "InterpreterSession.hpp"
#include "../support/InterpreterError.hpp"

#include <algorithm>
#include <iostream>

namespace interpreter {

InterpreterSession::InterpreterSession(StringPool& pool,
                                       DiagnosticEngine& diag,
                                       const InterpreterOptions& options)
    : m_pool(pool)
    , m_diag(diag)
    , m_options(options)
    , m_jit(pool) {
    // m_instanceTable is sized in initialize(), not here. See the
    // address-stability comment on the member in the header — the
    // vector's data() pointer is registered with the JIT, so sizing
    // must be coupled to that registration, which lives in initialize().
    //
    // Sizing it here as well would be redundant and, worse, would
    // invite a future reader to add a resize elsewhere on the
    // assumption that the constructor "owns" the initial sizing.
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

        // Reset and register instance table as absolute symbol. This is
        // the ONLY place m_instanceTable is sized; the address must
        // remain stable for the lifetime of the JIT registration below.
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

    // Instance table slots are cleared. The vector itself is NOT resized
    // or reallocated — that's deferred to the next initialize(), which
    // will re-register the (possibly same, possibly different) address
    // with the JIT. Between shutdown() and the next initialize(), no JIT
    // code exists to dereference the old address, so leaving the buffer
    // allocated is safe and avoids a needless free/realloc cycle.
    std::fill(m_instanceTable.begin(), m_instanceTable.end(), nullptr);
    m_initialized = false;
}

void InterpreterSession::registerLibrarySymbols(const std::string& path, const std::string& name) {
    m_jit.registerLibrarySymbols(path, name);
}

} // namespace interpreter