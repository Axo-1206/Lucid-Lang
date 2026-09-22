/// @file jit-runner/JITProgram.hpp
/// @brief Internal per-program state for the JIT runner.
///
/// ─── What This Class Is ───────────────────────────────────────────────
/// One loaded program. It holds the `CodegenResult` the runner is
/// currently running: the `llvm::Module`, its `LLVMContext`, the
/// `Manifest`, and the `ResourceTrackerSP` the JIT returned when the
/// module was added.
///
/// ─── What This Class Is NOT ───────────────────────────────────────────
/// It is not part of the public API. The public entry point is
/// `JITRunner`, in `JITRunner.hpp`. `JITProgram` is an implementation
/// detail; its header is not installed and its methods are not
/// referenced outside `jit-runner/`.
///
/// It is not a repository of module state. The state lives in the JIT'd
/// code (`@__module_state_<sanitized>` globals in the module). The
/// `JITProgram` holds the module, not its state.
///
/// ─── The Four Sequences ───────────────────────────────────────────────
/// The class implements four sequences:
///
///   - `create(session, diag, codegenResult)` — install a new program
///     from scratch. Load foreign libraries, add the module to the JIT,
///     run the program initializer.
///
///   - `run(session)` — invoke the entry point.
///
///   - `teardown(session)` — free the program's resources, quarantine
///     the module. Idempotent.
///
///   - (Reload is `teardown` followed by `create`, orchestrated by
///     `JITRunner::load`. There is no separate `reload` method on
///     `JITProgram` — the sequence is identical to load after the
///     teardown of the previous program.)
///
/// ─── Lifetime ─────────────────────────────────────────────────────────
/// `JITProgram` is created by `JITRunner::load` and destroyed by
/// `JITRunner::load` (before installing a new one) or by
/// `JITRunner::close`. It does not own the JIT session; the session is
/// passed in on every method that needs it.
///
/// The class is non-copyable and non-movable. It owns a
/// `CodegenResult`, which owns an `llvm::Module` and an `LLVMContext`.

#pragma once

#include "JITRunner.hpp"
#include "support/ExecutionResult.hpp"
#include "support/JITRunnerError.hpp"

#include "codegen/CodeGen.hpp"

#include "core/diagnostics/Diagnostic.hpp"

#include "jit/JITSession.hpp"

#include <llvm/ExecutionEngine/Orc/Core.h>   // ResourceTrackerSP
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <vector>

namespace jit_runner {

/// @brief One loaded program in the JIT runner.
class JITProgram {
public:
    /// @brief Install a new program.
    ///
    /// Loads the foreign libraries named in the manifest, adds the
    /// module to the JIT, and runs the program initializer.
    ///
    /// Throws `JITRunnerError` on failure. Diagnostics are emitted to
    /// the engine before the throw.
    ///
    /// Returns `nullptr` only if `JITProgram::create` is invoked with a
    /// `CodegenResult` whose `success == false`; all other failure paths
    /// throw. Callers should not rely on the null return — the normal
    /// contract is "either returns a valid program or throws."
    static std::unique_ptr<JITProgram> create(
        JITSession& session,
        DiagnosticEngine& diag,
        codegen::CodegenResult result);

    ~JITProgram();

    JITProgram(const JITProgram&) = delete;
    JITProgram& operator=(const JITProgram&) = delete;
    JITProgram(JITProgram&&) = delete;
    JITProgram& operator=(JITProgram&&) = delete;

    /// @brief Invoke the entry point.
    ///
    /// Throws `JITRunnerError` if the manifest has no entry symbol, or
    /// if the symbol cannot be resolved in the JIT.
    ExecutionResult run(JITSession& session);

    /// @brief Free the program's resources and remove its module from
    ///        the JIT.
    ///
    /// Idempotent. After `teardown`, the program is in an empty state:
    /// no module, no tracker, no resources. `run` throws if called.
    void teardown(JITSession& session);

private:
    JITProgram() = default;

    // ─── State ────────────────────────────────────────────────────────────

    /// The codegen output for this program.
    ///
    /// Owns the `llvm::Module` and `LLVMContext`. The module was moved
    /// into the JIT at `create` time; `m_codegenResult.module` is null
    /// after `create` returns. The context is retained (the JIT needs it
    /// alive for the duration of the module's presence in the JIT).
    codegen::CodegenResult m_codegenResult;

    /// The JIT's handle for the module. `create` installs the module
    /// under this tracker; `teardown` releases it. Null when no module
    /// is installed.
    llvm::orc::ResourceTrackerSP m_tracker;

    /// Foreign libraries the program loaded. Stored so `teardown` could
    /// unload them if the design ever requires it; today it doesn't, and
    /// the list is used only for diagnostic reporting.
    std::vector<std::string> m_loadedLibraries;

    /// The AST modules the program was generated from, for diagnostic
    /// purposes (source file paths in error messages). Empty for a
    /// program built from a `.bc` file, which has no AST.
    std::vector<ModuleAST*> m_moduleAsts;
};

} // namespace jit_runner