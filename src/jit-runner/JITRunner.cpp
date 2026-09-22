/// @file jit-runner/JITRunner.cpp
/// @brief Implementation of the JIT runner facade.

#include "JITRunner.hpp"
#include "JITProgram.hpp"
#include "jit/JITSession.hpp"
#include "support/JITRunnerError.hpp"

#include <cassert>

namespace jit_runner {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

JITRunner::JITRunner(StringPool& pool,
                     DiagnosticEngine& diag,
                     const JITRunnerOptions& options)
    : m_pool(pool)
    , m_diag(diag)
    , m_options(options)
{
}

JITRunner::~JITRunner() {
    // Tear down the current program while the JIT session is still
    // alive. If the session has already been destroyed (which cannot
    // happen here — m_session is destroyed after m_program because
    // it is declared first), teardown would be unsafe.
    //
    // The runner does not call close() during destruction if the
    // session was never initialized; there is nothing to tear down.
    if (m_program && m_session) {
        try {
            m_program->teardown(*m_session);
        } catch (...) {
            // Destructors do not throw. If teardown throws (which it
            // shouldn't — it calls `__lucid_program_free`, which does
            // not throw), log and swallow.
            //
            // The alternative — allowing the exception to propagate out
            // of ~JITRunner — would terminate the process, which is
            // worse for a CLI trying to exit cleanly.
        }
        m_program.reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void JITRunner::initialize() {
    if (m_session) return;   // idempotent

    m_session = std::make_unique<JITSession>(m_pool, m_diag, m_options);

    try {
        m_session->initialize();
    } catch (const std::exception& e) {
        m_session.reset();
        throw JITRunnerError(
            JITRunnerErrorKind::InitFailed,
            std::string("JIT session initialization failed: ") + e.what());
    }

    // Preload the kernel library, if one was named. It is loaded before
    // any manifest-declared foreign library, so kernel symbols are
    // available to every module.
    if (!m_options.kernelLibraryPath.empty()) {
        try {
            m_session->loadLibrary(m_options.kernelLibraryPath);
        } catch (const std::exception& e) {
            m_session.reset();
            throw JITRunnerError(
                JITRunnerErrorKind::LibraryLoadFailed,
                std::string("kernel library load failed: ") + e.what());
        }
    }
}

bool JITRunner::isInitialized() const {
    return m_session && m_session->isInitialized();
}

// ─────────────────────────────────────────────────────────────────────────────
// Load / Reload
// ─────────────────────────────────────────────────────────────────────────────

void JITRunner::load(codegen::CodegenResult result) {
    if (!m_session) {
        initialize();
    }

    // ─── Validate the CodegenResult ───────────────────────────────────────
    if (!result.success) {
        throw JITRunnerError(
            JITRunnerErrorKind::InvalidCodegenResult,
            "CodegenResult has success == false");
    }
    if (!result.module) {
        throw JITRunnerError(
            JITRunnerErrorKind::InvalidCodegenResult,
            "CodegenResult has a null module");
    }
    if (result.manifest.programInitSymbol.empty()) {
        throw JITRunnerError(
            JITRunnerErrorKind::InvalidCodegenResult,
            "CodegenResult's manifest has no program init symbol");
    }

    // ─── Tear down any existing program ───────────────────────────────────
    // The old program's resources are freed before the new module is
    // installed. This is required for correctness: `__lucid_program_free`
    // is a symbol in the old module, and it is only resolvable while the
    // old module is in the JIT.
    if (m_program) {
        m_program->teardown(*m_session);
        m_program.reset();
    }

    // ─── Install the new program ──────────────────────────────────────────
    m_program = JITProgram::create(*m_session, m_diag, std::move(result));
    if (!m_program) {
        // JITProgram::create throws on failure; reaching here would be a
        // bug in create's contract. Defensive.
        throw JITRunnerError(
            JITRunnerErrorKind::ModuleAddFailed,
            "JITProgram::create returned null without throwing");
    }
}

void JITRunner::reload(codegen::CodegenResult result) {
    // Reload is semantically the same as load in Tier 1: the old
    // program's resources are freed, the new module is installed, its
    // initializer runs. There is no state preservation.
    //
    // The two methods are kept separate for intent and for future
    // divergence: when Tier 2 state preservation lands, `reload` will
    // consult `manifest.programMigrateSymbol` and call the migration
    // function instead of the fresh init. `load` will continue to run
    // the fresh init unconditionally.
    load(std::move(result));
}

// ─────────────────────────────────────────────────────────────────────────────
// Run
// ─────────────────────────────────────────────────────────────────────────────

ExecutionResult JITRunner::run() {
    if (!m_session || !m_session->isInitialized()) {
        throw JITRunnerError(
            JITRunnerErrorKind::InitFailed,
            "JIT runner is not initialized");
    }
    if (!m_program) {
        throw JITRunnerError(
            JITRunnerErrorKind::EntrySymbolNotFound,
            "no program is loaded");
    }

    return m_program->run(*m_session);
}

// ─────────────────────────────────────────────────────────────────────────────
// Teardown
// ─────────────────────────────────────────────────────────────────────────────

void JITRunner::close() {
    if (!m_program) return;   // idempotent

    m_program->teardown(*m_session);
    m_program.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

JITSession& JITRunner::session() {
    assert(m_session && "session() called before initialize()");
    return *m_session;
}

const JITSession& JITRunner::session() const {
    assert(m_session && "session() called before initialize()");
    return *m_session;
}

} // namespace jit_runner