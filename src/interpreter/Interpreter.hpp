/// @file Interpreter.hpp
/// @brief Main interpreter API.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/memory/InternedString.hpp"
#include "support/InterpreterOptions.hpp"
#include "support/ExecutionResult.hpp"
#include "core/InterpreterContext.hpp"

#include <vector>
#include <memory>

namespace interpreter {

/// @brief Main interpreter engine.
///
/// This is the public API for the interpreter. It orchestrates all components
/// to parse, compile, and execute Lucid programs.
///
/// @example
/// @code
/// StringPool pool;
/// DiagnosticEngine diag;
/// Interpreter interpreter(pool, diag);
///
/// InterpreterOptions options;
/// options.entryPoint = "main";
/// interpreter.initialize(options);
///
/// ModuleAST* module = parseModule("main.luc");
/// ExecutionResult result = interpreter.run(module);
/// @endcode
class Interpreter {
public:
    /// @brief Construct an interpreter with shared resources.
    Interpreter(StringPool& pool, DiagnosticEngine& diagnostics);
    ~Interpreter() = default;

    // ─── Initialization ─────────────────────────────────────────────────

    /// @brief Initialize the interpreter.
    /// @param options Configuration options.
    /// @throws InterpreterError if initialization fails.
    void initialize(const InterpreterOptions& options = InterpreterOptions{});

    /// @brief Check if initialized.
    bool isInitialized() const { return m_context && m_context->initialized; }

    // ─── Execution ──────────────────────────────────────────────────────

    /// @brief Run a single module.
    /// @param module The AST module to run.
    /// @param entryPoint Override entry point (empty = use options).
    /// @return Execution result.
    /// @throws InterpreterError if execution fails.
    ExecutionResult run(ModuleAST* module, InternedString entryPoint = InternedString());

    /// @brief Run a single module with string entry point.
    ExecutionResult run(ModuleAST* module, const std::string& entryPoint = "");

    /// @brief Run multiple modules.
    /// @param modules The AST modules to run (in dependency order).
    /// @param entryPoint Override entry point (empty = use options).
    /// @return Execution result.
    /// @throws InterpreterError if execution fails.
    ExecutionResult run(const std::vector<ModuleAST*>& modules,
                        InternedString entryPoint = InternedString());

    /// @brief Run multiple modules with string entry point.
    ExecutionResult run(const std::vector<ModuleAST*>& modules,
                        const std::string& entryPoint = "");

    // ─── Loading ────────────────────────────────────────────────────────

    /// @brief Load a module without executing.
    /// @param module The AST module to load.
    /// @return true on success.
    /// @throws InterpreterError if loading fails.
    bool load(ModuleAST* module);

    /// @brief Load multiple modules.
    /// @param modules The AST modules to load.
    /// @return true on success.
    /// @throws InterpreterError if loading fails.
    bool load(const std::vector<ModuleAST*>& modules);

    // ─── Hot-Reload ─────────────────────────────────────────────────────

    /// @brief Hot-reload a module.
    /// @param module The new AST module.
    /// @param name The module name to replace.
    /// @return true on success.
    /// @throws InterpreterError if hot-reload fails.
    bool hotReload(ModuleAST* module, InternedString name);

    /// @brief Hot-reload with string name.
    bool hotReload(ModuleAST* module, const std::string& name);

    // ─── Foreign Libraries ─────────────────────────────────────────────

    /// @brief Register a foreign library.
    /// @param name The library name.
    /// @throws InterpreterError if loading fails.
    void registerLibrary(const std::string& name);

    /// @brief Register libraries from a module.
    void registerLibraries(ModuleAST* module);

    /// @brief Register libraries from multiple modules.
    void registerLibraries(const std::vector<ModuleAST*>& modules);

    // ─── Symbol Lookup ──────────────────────────────────────────────────

    /// @brief Look up a symbol in the JIT.
    /// @param name The symbol name.
    /// @return Pointer to the symbol, or nullptr if not found.
    void* lookupSymbol(const std::string& name);

    /// @brief Look up a symbol by InternedString.
    void* lookupSymbol(InternedString name);

    // ─── Accessors ──────────────────────────────────────────────────────

    /// @brief Get the interpreter context.
    InterpreterContext& getContext() { return *m_context; }

    /// @brief Get the interpreter context (const).
    const InterpreterContext& getContext() const { return *m_context; }

    /// @brief Get the options.
    const InterpreterOptions& getOptions() const { return m_context->options; }

    /// @brief Get the loaded modules.
    std::vector<ModuleInfo*> getLoadedModules();

private:
    std::unique_ptr<InterpreterContext> m_context;

    /// @brief Internal run implementation.
    ExecutionResult runImpl(const std::vector<ModuleAST*>& modules,
                            InternedString entryPoint);

    /// @brief Find the entry point.
    InternedString findEntryPoint(const std::vector<ModuleAST*>& modules,
                                  InternedString entryPoint);

    /// @brief Register libraries from a module.
    void registerLibrariesFromModule(ModuleAST* module);
};

} // namespace interpreter