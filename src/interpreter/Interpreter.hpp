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

    /// @brief Load the program.
    bool load(const std::vector<ModuleAST*>& modules);

    /// @brief Reload one or more modules in place.
    bool reload(const std::vector<ModuleAST*>& modules);

    /// @brief Hot-reload a changed module and all its dependents.
    bool hotReload(ModuleAST* module, InternedString name);

    /// @brief Execute the entry point.
    ExecutionResult run(InternedString entryPoint = InternedString());

    /// @brief Access underlying session.
    InterpreterSession& session() { return *m_session; }
    const InterpreterSession& session() const { return *m_session; }

    /// @brief Access underlying loaded program (or nullptr if none).
    InterpreterProgram* program() { return m_program.get(); }
    const InterpreterProgram* program() const { return m_program.get(); }

private:
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

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, InternedString name);
bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name);

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx);

} // namespace interpreter