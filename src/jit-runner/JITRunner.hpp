/// @file jit-runner/JITRunner.hpp
/// @brief The JIT runner — the one public entry point of the jit-runner
///        backend.
///
/// ─── What This File Is ────────────────────────────────────────────────
/// The public API of the JIT backend. A `JITRunner` owns a JIT session
/// and, at any given time, one loaded program. It accepts a
/// `codegen::CodegenResult`, compiles it to native machine code in
/// memory, and runs it.
///
/// ─── What This File Is NOT ────────────────────────────────────────────
/// It is NOT an interpreter. There is no interpretation. The module is
/// compiled to machine code and invoked. The folder name `jit-runner`
/// reflects this; the older `interpreter` name was a misnomer.
///
/// It is NOT a compiler driver. It does not parse, sema, or codegen.
/// Its entire input is a `CodegenResult`.
///
/// It is NOT a file watcher or version tracker. It does not know about
/// source files, file mtimes, or which modules have changed. The CLI
/// owns those concerns; the runner is told "here is a program, run it"
/// or "here is a new program, replace the old one."
///
/// ─── The API ──────────────────────────────────────────────────────────
/// Five methods:
///
///   - `initialize()` — set up the JIT session. Idempotent.
///   - `load(CodegenResult)` — replace any existing program with a new
///     one. Frees the old program's resources first.
///   - `reload(CodegenResult)` — same as `load`, but semantically
///     "replace the current program"; delegates to `load` when no
///     program is loaded.
///   - `run()` — invoke the entry point and return its exit code.
///   - `close()` — tear down the current program. Idempotent.
///
/// ─── What "Program" Means ─────────────────────────────────────────────
/// A program is one `CodegenResult`: an `llvm::Module`, its LLVM
/// context, and a `Manifest`. The runner holds at most one program at a
/// time. Loading a new one replaces the old.
///
/// ─── Reload Semantics ─────────────────────────────────────────────────
/// Reload is a **full replace**. The old program's resources are
/// released (`__lucid_program_free`), the new module is installed, and
/// the new program's initializer runs (`__lucid_program_init`). State
/// from the old program does not survive.
///
/// Preserving state across a reload is a codegen feature — the codegen
/// step would emit a migration function alongside the fresh init, and
/// the manifest would name it. The runner's API doesn't change for
/// that; it just calls whichever symbol the manifest names.
///
/// ─── Lifecycle ────────────────────────────────────────────────────────
/// The runner must be initialized before `load` or `reload`.
/// `initialize` is idempotent, and `load` calls it if needed. `close`
/// tears down the current program but leaves the JIT session up; a
/// subsequent `load` reuses the session.
///
/// The runner is non-copyable and non-movable. Its lifetime is the
/// CLI's `run` command.

#pragma once

#include "JITRunnerOptions.hpp"
#include "support/ExecutionResult.hpp"

#include "codegen/CodeGen.hpp"

#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"

#include <memory>

namespace jit_runner {

class JITSession;
class JITProgram;

class JITRunner {
public:
    /// @brief Construct a runner bound to a string pool and diagnostic
    ///        engine. Both must outlive the runner.
    JITRunner(StringPool& pool,
              DiagnosticEngine& diag,
              const JITRunnerOptions& options = JITRunnerOptions{});

    ~JITRunner();

    JITRunner(const JITRunner&) = delete;
    JITRunner& operator=(const JITRunner&) = delete;
    JITRunner(JITRunner&&) = delete;
    JITRunner& operator=(JITRunner&&) = delete;

    // ─── Lifecycle ────────────────────────────────────────────────────────

    /// @brief Initialize the JIT session.
    ///
    /// Idempotent. `load` and `reload` call this automatically if it
    /// has not been called yet, so the CLI does not have to.
    ///
    /// Throws `JITRunnerError` (InitFailed) on failure. Diagnostics are
    /// emitted to the engine before the throw.
    void initialize();

    /// @brief True if `initialize` has been called and succeeded.
    bool isInitialized() const;

    // ─── Load / Reload ────────────────────────────────────────────────────

    /// @brief Load a program from a `CodegenResult`.
    ///
    /// If a program is already loaded, it is freed first (via
    /// `__lucid_program_free`), then the new module is installed and its
    /// initializer runs. There is no window where two programs are both
    /// live in the JIT.
    ///
    /// The `CodegenResult` is consumed (moved). Its `llvm::Module` and
    /// `LLVMContext` live until the next `load` / `reload` / `close`, or
    /// until the runner is destroyed.
    ///
    /// Throws `JITRunnerError` on failure. Diagnostics are emitted to
    /// the engine before the throw.
    void load(codegen::CodegenResult result);

    /// @brief Replace the current program with a new one.
    ///
    /// Semantically identical to `load`. The distinction is intent: the
    /// CLI calls `load` on first run and `reload` on a hot-reload.
    /// Because the runner does not track "which run this is," the two
    /// do the same thing.
    void reload(codegen::CodegenResult result);

    // ─── Run ──────────────────────────────────────────────────────────────

    /// @brief Invoke the entry point and return its exit code.
    ///
    /// Throws `JITRunnerError` (EntrySymbolNotFound) if the loaded
    /// program's manifest has no entry symbol, or if the symbol cannot
    /// be resolved in the JIT. Diagnostics are emitted before the throw.
    ///
    /// A panic inside the JIT'd code terminates the process; it does
    /// not surface as an exception here. See `RuntimeError.hpp` and
    /// `PanicRuntime.cpp` for the panic path.
    ExecutionResult run();

    // ─── Teardown ─────────────────────────────────────────────────────────

    /// @brief Tear down the current program, if any. Idempotent.
    ///
    /// Frees the program's resources (`__lucid_program_free`), removes
    /// the module's symbols from the JIT, and releases the module. The
    /// JIT session itself stays alive; a subsequent `load` reuses it.
    ///
    /// Safe to call multiple times. Safe to call on a runner that has
    /// never loaded a program.
    void close();

    // ─── Accessors ────────────────────────────────────────────────────────

    /// @brief The JIT session. Exposed for advanced tooling (REPL, LSP).
    ///        Most callers should use the facade methods above.
    JITSession& session();
    const JITSession& session() const;

private:
    StringPool& m_pool;
    DiagnosticEngine& m_diag;
    JITRunnerOptions m_options;

    std::unique_ptr<JITSession> m_session;
    std::unique_ptr<JITProgram> m_program;
};

} // namespace jit_runner