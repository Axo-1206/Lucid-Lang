/// @file Interpreter.hpp
/// @brief Main interpreter API and Facade.

#pragma once

#include "core/InterpreterSession.hpp"
#include "core/InterpreterProgram.hpp"
#include "core/InterpreterContext.hpp"
#include "support/InterpreterOptions.hpp"
#include "support/ExecutionResult.hpp"

#include <vector>
#include <memory>

namespace interpreter {

// =============================================================================
// Interpreter Facade Class
// =============================================================================

/// @brief High-level facade managing an interpreter session and loaded program.
///
/// ─── Session and Program Lifetime ────────────────────────────────────
/// The Interpreter owns both the InterpreterSession and the
/// InterpreterProgram. The session is declared first, so it is destroyed
/// last (members are destroyed in reverse declaration order). The
/// destructor body calls closeProgram() to tear down the program while
/// the session is still alive; the program's own destructor then only
/// asserts that teardown happened.
///
/// See InterpreterProgram's class doc comment for the full rationale on
/// why the program does not hold a reference to the session and why
/// teardown is owner-driven.
class Interpreter {
public:
    Interpreter(StringPool& pool, DiagnosticEngine& diag,
                const InterpreterOptions& options = InterpreterOptions{});
    ~Interpreter();

    // Non-copyable
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    /// @brief Initialize the session.
    void initialize();

    /// @brief Check if initialized.
    bool isInitialized() const;

    /// @brief Load the program. Replaces any previously-loaded program,
    ///        tearing it down first.
    /// @return true on success, false if any module had semantic errors.
    bool load(const std::vector<ModuleAST*>& modules);

    /// @brief Reload one or more modules in place.
    ///
    /// If no program is currently loaded, delegates to load().
    /// If a program is loaded, performs a hot-reload of the given
    /// modules using the program's reload() (see InterpreterProgram's
    /// docs for the atomicity contract).
    ///
    /// @return true if reload succeeded.
    bool reload(const std::vector<ModuleAST*>& modules);

    /// @brief Hot-reload a changed module and all its dependents.
    ///
    /// Returns false if the module is null, if hot-reload is disabled in
    /// the session options, or if the underlying reload fails. Throws
    /// only if the program's reload throws (which it does not, under
    /// current implementation).
    bool hotReload(ModuleAST* module, InternedString name);

    /// @brief Execute the entry point.
    /// @return ExecutionResult with success = (exitCode == 0).
    ExecutionResult run(InternedString entryPoint = InternedString());

    /// @brief Access underlying session.
    ///
    /// The non-const overload exposes the full session, allowing callers
    /// to mutate options, the instance table, and other internals. This
    /// is intended for advanced use (LSP integration, tooling); most
    /// callers should use the Interpreter facade's own methods instead.
    InterpreterSession& session() { return *m_session; }
    const InterpreterSession& session() const { return *m_session; }

    /// @brief Access underlying loaded program (or nullptr if none).
    InterpreterProgram* program() { return m_program.get(); }
    const InterpreterProgram* program() const { return m_program.get(); }

private:
    /// Tear down the currently-loaded program (if any) while the session
    /// is still alive, then reset the pointer. Called by the destructor
    /// and by load() before replacing an existing program.
    void closeProgram();

    std::unique_ptr<InterpreterSession> m_session;
    std::unique_ptr<InterpreterProgram> m_program;
};

// =============================================================================
// Procedural Convenience API
// =============================================================================

void initialize(InterpreterContext& ctx, 
                const InterpreterOptions& options = InterpreterOptions{});

bool isInitialized(const InterpreterContext& ctx);

ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint = InternedString(),
                           bool isHotReload = false);

ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          InternedString entryPoint = InternedString(),
                          bool isHotReload = false);

bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules);
bool loadModule(InterpreterContext& ctx, ModuleAST* module);

/// @brief Hot-reload a module by InternedString name.
///
/// Returns false if the module is null, if hot-reload is disabled in
/// either the context or session options, or if the underlying reload
/// fails. Throws only for catastrophic failures that the program's
/// reload propagates.
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, InternedString name);

/// @brief Hot-reload a module by std::string name (interned on the way in).
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name);

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx);

} // namespace interpreter