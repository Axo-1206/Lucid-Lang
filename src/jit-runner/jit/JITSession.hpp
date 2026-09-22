/// @file jit-runner/jit/JITSession.hpp
/// @brief ORC JIT session management.
///
/// ─── What This Class Is ───────────────────────────────────────────────
/// A thin wrapper around LLVM's ORC `LLJIT`. It owns the JIT engine,
/// the host target machine, the platform symbol generator, and the
/// set of foreign libraries that have been loaded. It exposes four
/// services:
///
///   - `addModule` — install a `ThreadSafeModule` in the JIT, get back
///     a `ResourceTrackerSP` identifying it.
///   - `lookupSymbol` — resolve a symbol name to a `void*` address.
///   - `quarantine` — remove a module's symbols from the JIT.
///   - `loadLibrary` — load a foreign library and make its symbols
///     resolvable to JIT'd code.
///
/// ─── What This Class Is NOT ───────────────────────────────────────────
/// It is NOT the runner. The runner (see `JITRunner.hpp`) orchestrates
/// load / reload / run / teardown and knows about `CodegenResult` and
/// `Manifest`. The session is a lower-level service that only knows
/// about LLVM modules and symbols.
///
/// It is NOT a module registry. It does not track which modules are
/// loaded by name, and it does not implement "reload by name." The
/// runner holds the current program's tracker and calls `quarantine`
/// with it when the program is replaced.
///
/// It is NOT a foreign-library manager in the "which libraries does
/// this program need" sense. That's the manifest's business. The
/// session loads libraries on request and tracks which paths have been
/// loaded for idempotence; it does not decide which to load.
///
/// ─── The Two LLVMContexts ─────────────────────────────────────────────
/// There are two `LLVMContext`s in play:
///
///   - The **codegen context**: created by `codegen::generate`, owned by
///     the `CodegenResult`, and the context the generated module lives
///     in. When the runner adds the module to the JIT, it *moves* the
///     context into the `ThreadSafeModule`. After `addModule`, the
///     context is owned by the JIT, not the codegen result.
///
///   - The **session context**: created by `JITSession::initialize`,
///     owned by the session. It is used for `mangleAndIntern` and any
///     other LLVM-level operation the session performs on its own. It
///     is *not* where program modules live.
///
/// These are deliberately separate. Before this rewrite, the session
/// created the context that codegen used, forcing the codegen result
/// to be created after the session and tying their lifetimes together.
/// The rewrite decouples them: codegen creates its own context, the
/// runner moves it into the JIT alongside the module, and the session
/// keeps its own separate context.
///
/// The two contexts are never mixed. A `Value*` from one cannot be used
/// in the other. `addModule` is the boundary: the module's context is
/// moved in with the module; the session's context is used only for
/// session-internal operations.

#pragma once

#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"
#include "jit-runner/JITRunnerOptions.hpp"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>

#include <memory>
#include <set>
#include <string>

namespace jit_runner {

/// @brief Exception type for JIT-level failures.
///
/// Distinct from `JITRunnerError` (in `support/JITRunnerError.hpp`),
/// which is the runner's public exception. `JITError` is internal to
/// the session and its immediate callers; `JITRunner` catches it,
/// emits a diagnostic, and rethrows as a `JITRunnerError`.
class JITError : public std::runtime_error {
public:
    enum class Kind {
        /// Initialization failed: LLVM target init, LLJIT creation, or
        /// platform symbol generator setup.
        InitFailed,

        /// `addModule` failed: module verification failed, or the
        /// JITDylib rejected the module.
        ModuleAddFailed,

        /// `lookupSymbol` failed for a reason other than "not found" —
        /// the resolver surfaced an error.
        LookupFailed,

        /// `loadLibrary` failed: the library was not found, or
        /// `dlopen` / `LoadLibrary` rejected it.
        LibraryLoadFailed,
    };

    JITError(Kind kind, const std::string& msg)
        : std::runtime_error(msg), m_kind(kind) {}

    Kind getKind() const { return m_kind; }

private:
    Kind m_kind;
};

/// @brief Wraps an ORC `LLJIT` and its associated services.
class JITSession {
public:
    JITSession(StringPool& pool,
               DiagnosticEngine& diag,
               const JITRunnerOptions& options);
    ~JITSession();

    JITSession(const JITSession&) = delete;
    JITSession& operator=(const JITSession&) = delete;
    JITSession(JITSession&&) = delete;
    JITSession& operator=(JITSession&&) = delete;

    // ─── Lifecycle ────────────────────────────────────────────────────────

    /// @brief Initialize the JIT: LLVM targets, LLJIT, platform symbol
    ///        generator.
    ///
    /// Idempotent. Throws `JITError` (InitFailed) on failure.
    void initialize();

    /// @brief True if `initialize` has been called and succeeded.
    bool isInitialized() const { return m_initialized; }

    // ─── Module Management ────────────────────────────────────────────────

    /// @brief Add a module to the JIT.
    ///
    /// The `ThreadSafeModule` carries both the module and its
    /// `LLVMContext`. On success, both are moved into the JIT; the
    /// caller's `ThreadSafeModule` is consumed and cannot be reused.
    ///
    /// Verification happens here, not in the caller, because the module
    /// is verified after the target triple and data layout are set —
    /// verification against the host's data layout catches size and
    /// alignment mismatches that a verification against an unset layout
    /// would not.
    ///
    /// The returned `ResourceTrackerSP` identifies the module. Pass it
    /// to `quarantine` to remove the module's symbols from the JIT.
    ///
    /// Throws `JITError` (ModuleAddFailed) if verification or addition
    /// fails. A diagnostic is emitted to the session's diagnostic engine
    /// before the throw.
    ///
    /// `name` is a diagnostic aid; it appears in LLVM error messages
    /// and in JITDylib dumps. It does not need to be unique across
    /// calls — the runner uses `"__lucid_program__"` for every program.
    llvm::orc::ResourceTrackerSP addModule(
        llvm::orc::ThreadSafeModule tsm,
        const std::string& name);

    /// @brief Quarantine a module — remove its symbols from the JIT.
    ///
    /// In single-threaded mode, removes the tracker immediately. The
    /// caller is responsible for ensuring no thread is executing in the
    /// module being removed.
    ///
    /// Idempotent for a null tracker. Does not throw: if the tracker's
    /// removal fails (it was already defunct, or a symbol is
    /// mid-materialization), logs a warning if verbose and returns.
    void quarantine(llvm::orc::ResourceTrackerSP tracker);

    // ─── Symbol Lookup ────────────────────────────────────────────────────

    /// @brief Look up a symbol in the JIT.
    ///
    /// Returns the symbol's address, or `nullptr` if the symbol is not
    /// found. This is the common case: the runner looks up the entry
    /// symbol and the program init/free symbols by name, and a missing
    /// symbol is a compiler bug the caller diagnoses.
    ///
    /// Throws `JITError` (LookupFailed) if the resolver itself fails —
    /// which is distinct from "symbol not found."
    void* lookupSymbol(const std::string& name);

    // ─── Foreign Libraries ────────────────────────────────────────────────

    /// @brief Load a foreign library and make its symbols resolvable to
    ///        JIT-compiled code.
    ///
    /// Idempotent: a library whose path has already been loaded is
    /// skipped.
    ///
    /// On Linux/macOS, calls `dlopen(path, RTLD_NOW | RTLD_GLOBAL)`.
    /// On Windows, calls `LoadLibraryA(path)`.
    ///
    /// The library remains loaded for the process's lifetime. Unloading
    /// is not supported — a symbol a JIT'd program resolved through the
    /// library would become dangling if the library were unloaded.
    ///
    /// Throws `JITError` (LibraryLoadFailed) if the library cannot be
    /// loaded. A diagnostic is emitted before the throw.
    void loadLibrary(const std::string& path);

    // ─── Accessors ────────────────────────────────────────────────────────

    /// @brief The session's own `LLVMContext`.
    ///
    /// NOT a context that program modules live in. See the file header's
    /// "two LLVMContexts" note. Callers should not create modules in
    /// this context; they should pass `ThreadSafeModule`s built from
    /// codegen output to `addModule`.
    llvm::LLVMContext& context() { return *m_context; }

    /// @brief The underlying ORC JIT.
    ///
    /// Exposed for advanced tooling — custom symbol resolvers, JIT
    /// dumps. Most callers should use the facade methods above.
    llvm::orc::LLJIT& jit() { return *m_jit; }

    /// @brief The string pool this session was constructed with.
    ///
    /// Used for symbol name interning in callers.
    StringPool& pool() { return m_pool; }

private:
    // ─── Setup ────────────────────────────────────────────────────────────

    /// Set up the JIT target machine and create the `LLJIT` instance.
    /// Called by `initialize`. Throws `JITError` (InitFailed) on failure.
    void setupTarget();

    /// Install the platform symbol generator on the main JITDylib, so
    /// process-wide symbols (the Lucid runtime, foreign libraries loaded
    /// via `loadLibrary`) are resolvable to JIT'd code.
    ///
    /// Called by `initialize`. Throws `JITError` (InitFailed) on failure.
    void setupPlatformSymbols();

    // ─── State ────────────────────────────────────────────────────────────

    StringPool& m_pool;
    DiagnosticEngine& m_diag;
    JITRunnerOptions m_options;

    /// The session's own `LLVMContext`. Used for `mangleAndIntern` and
    /// other session-internal operations. Program modules do not live
    /// in this context — they bring their own, moved in with the
    /// `ThreadSafeModule`.
    std::unique_ptr<llvm::LLVMContext> m_context;

    /// The ORC JIT engine. Null until `initialize` succeeds.
    std::unique_ptr<llvm::orc::LLJIT> m_jit;

    /// Paths of libraries loaded via `loadLibrary`. Used for idempotence.
    std::set<std::string> m_loadedLibraries;

    bool m_initialized = false;
};

} // namespace jit_runner