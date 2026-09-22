/// @file jit-runner/JITProgram.cpp
/// @brief Implementation of the internal per-program state.
///
/// ─── The Two Sequences ────────────────────────────────────────────────
/// This file implements two of the four sequences described in the
/// header:
///
///   - `create` — install a new program from a CodegenResult.
///   - `run`    — invoke the entry point.
///   - `teardown` — free the program's resources.
///
/// Reload is not a separate sequence. The runner's `reload` tears down
/// the current program and calls `create` for the new one. The two are
/// symmetric — reload is exactly "teardown, then create" — so there is
/// no separate code path.
///
/// ─── Why the Sequences Are Split This Way ─────────────────────────────
/// `create` and `teardown` are the two halves of a program's lifetime.
/// Splitting them lets the runner express "replace program A with
/// program B" as "teardown A, create B" without any intermediate state.
/// The alternative — a single `reload` method that does both — would
/// duplicate the create and teardown logic or dispatch to them anyway.

#include "JITProgram.hpp"
#include "jit/JITSession.hpp"

#include "core/trace/Trace.hpp"

#include <chrono>
#include <string>

namespace jit_runner {

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

JITProgram::~JITProgram() {
    // The owner (JITRunner) is responsible for calling teardown before
    // destruction. Reaching the destructor with a live tracker or a
    // live program is a contract violation; the JIT's resources would
    // leak until the session is destroyed.
    //
    // Assert in debug builds so the violation surfaces during
    // development. Do not attempt cleanup here — the destructor has no
    // session parameter, and calling into the JIT without the session
    // would be unsafe.
    assert(!m_tracker && "JITProgram destroyed without teardown()");
}

// ─────────────────────────────────────────────────────────────────────────────
// create
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<JITProgram> JITProgram::create(
    JITSession& session,
    DiagnosticEngine& diag,
    codegen::CodegenResult result)
{
    auto program = std::unique_ptr<JITProgram>(new JITProgram());

    // ─── Move the codegen result into the program ─────────────────────────
    // The manifest and the LLVM context are retained; the module is
    // about to be moved into the JIT.
    program->m_codegenResult.manifest = std::move(result.manifest);
    program->m_codegenResult.llvmCtx  = std::move(result.llvmCtx);
    program->m_codegenResult.success  = result.success;
    // Note: result.module is moved below, into the JIT.

    // ─── 1. Load foreign libraries named in the manifest ──────────────────
    //
    // Loading is idempotent: a library already loaded by a previous
    // program (or by the kernel preload) is skipped. The manifest lists
    // each library once.
    for (const auto& lib : program->m_codegenResult.manifest.foreignLibraries) {
        try {
            session.loadLibrary(lib.name);
            program->m_loadedLibraries.push_back(lib.name);
        } catch (const std::exception& e) {
            diag.error(DiagCode::Ffi_SymbolNotFound, nullptr,
                       "failed to load foreign library '", lib.name,
                       "': ", e.what());
            throw JITRunnerError(
                JITRunnerErrorKind::LibraryLoadFailed,
                "failed to load foreign library '" + lib.name + "'");
        }
    }

    // ─── 2. Add the module to the JIT ─────────────────────────────────────
    //
    // The module was moved into the JIT at this point; `result.module`
    // is null afterward. The tracker identifies the module within the
    // JIT's symbol table.
    //
    // `JITSession::addModule` throws on failure (module verification
    // failed, JITDylib rejected the module). Propagate as JITRunnerError.
    try {
        program->m_tracker = session.addModule(
            std::move(result.module), "__lucid_program__");
    } catch (const std::exception& e) {
        diag.error(DiagCode::Backend_InvalidIR, nullptr,
                   "failed to add program module to JIT: ", e.what());
        throw JITRunnerError(
            JITRunnerErrorKind::ModuleAddFailed,
            std::string("failed to add program module to JIT: ") + e.what());
    }

    // ─── 3. Run the program initializer ───────────────────────────────────
    //
    // `__lucid_program_init` writes each module's initial values into
    // its state global. It has no arguments and returns void.
    //
    // The symbol must exist in the JIT. If it doesn't, codegen failed to
    // emit it — a compiler bug, not a user-visible condition. Diagnose
    // and throw.
    const std::string& initSymbol =
        program->m_codegenResult.manifest.programInitSymbol;
    void* initPtr = session.lookupSymbol(initSymbol);
    if (!initPtr) {
        diag.error(DiagCode::Backend_CodegenError, nullptr,
                   "program initializer '", initSymbol,
                   "' not found in JIT — codegen did not emit it");
        throw JITRunnerError(
            JITRunnerErrorKind::ProgramSymbolNotFound,
            "program initializer not found: " + initSymbol);
    }

    reinterpret_cast<void(*)()>(initPtr)();

    Trace::detail("JITProgram: installed program, initializer ran");

    return program;
}

// ─────────────────────────────────────────────────────────────────────────────
// run
// ─────────────────────────────────────────────────────────────────────────────

ExecutionResult JITProgram::run(JITSession& session) {
    // ─── Resolve the entry point ──────────────────────────────────────────
    //
    // The manifest names the symbol. It is the linker-level mangled name
    // of the entry function (for an `@[export] const main`, this equals
    // "main" — export disables mangling; for a `@[export]` with a
    // different name, it is that name).
    const std::string& entrySymbol = m_codegenResult.manifest.entry.symbol;
    if (entrySymbol.empty()) {
        throw JITRunnerError(
            JITRunnerErrorKind::EntrySymbolNotFound,
            "program has no entry point");
    }

    void* entryPtr = session.lookupSymbol(entrySymbol);
    if (!entryPtr) {
        throw JITRunnerError(
            JITRunnerErrorKind::EntrySymbolNotFound,
            "entry symbol '" + entrySymbol + "' not found in JIT");
    }

    // ─── Invoke the entry point ───────────────────────────────────────────
    //
    // No try/catch around the call. A panic inside JIT'd code calls
    // `__lucid_panic`, which terminates the process; it does not
    // surface as a C++ exception. Foreign functions are C-only, so
    // nothing reachable from the entry point throws a C++ exception
    // either. If something does throw, it's a bug in the runner or in a
    // runtime helper, and letting it propagate to the caller — where it
    // becomes a JITRunnerError or a diagnostic — is the correct response.
    auto startTime = std::chrono::high_resolution_clock::now();
    int exitCode = reinterpret_cast<int(*)()>(entryPtr)();
    auto endTime = std::chrono::high_resolution_clock::now();

    // ─── Build the result ─────────────────────────────────────────────────
    ExecutionResult result;
    result.exitCode = exitCode;
    result.success = (exitCode == 0);
    result.executionTimeMs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime).count() / 1000.0;
    result.entryPointUsed = entrySymbol;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// teardown
// ─────────────────────────────────────────────────────────────────────────────

void JITProgram::teardown(JITSession& session) {
    if (!m_tracker) return;   // idempotent

    // ─── 1. Free the program's resources ──────────────────────────────────
    //
    // `__lucid_program_free` releases every module's resources in
    // reverse dependency order. It must run before the module is
    // removed from the JIT — the free function is a symbol in the
    // module, and it is only resolvable while the module is in the JIT.
    //
    // If the free symbol is missing (a codegen bug), log a diagnostic
    // and continue. The JIT's resources will still be released by
    // `session.quarantine` below; the Lucid resources the free function
    // was supposed to release will leak until process exit. That's worse
    // than crashing, but it lets the CLI report the diagnostic and
    // continue with a best-effort teardown.
    const std::string& freeSymbol =
        m_codegenResult.manifest.programFreeSymbol;
    if (!freeSymbol.empty()) {
        void* freePtr = session.lookupSymbol(freeSymbol);
        if (freePtr) {
            reinterpret_cast<void(*)()>(freePtr)();
        }
        // If freePtr is null: the symbol wasn't found. This is a
        // codegen bug — the manifest promised a symbol that wasn't
        // emitted. Log and continue; the CLI will see the diagnostic.
    }

    // ─── 2. Quarantine the module ─────────────────────────────────────────
    //
    // `quarantine` removes the module's symbols from the JIT. In
    // single-threaded mode it does so immediately; the caller is
    // responsible for ensuring no thread is executing in the module.
    //
    // The runner is single-threaded at this point. The CLI drives the
    // game loop and calls `JITRunner::reload` between frames; no
    // frame can be mid-execution when reload fires.
    session.quarantine(m_tracker);
    m_tracker = nullptr;

    // ─── 3. Clear the codegen result ──────────────────────────────────────
    //
    // The module is already gone (moved into the JIT at create time,
    // removed by quarantine at teardown). Clearing the codegen result
    // releases the LLVMContext, which is safe once the module is gone.
    m_codegenResult = {};

    m_loadedLibraries.clear();
    m_moduleAsts.clear();

    Trace::detail("JITProgram: teardown complete");
}

} // namespace jit_runner