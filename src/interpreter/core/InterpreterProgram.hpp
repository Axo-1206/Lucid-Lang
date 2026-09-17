/// @file core/InterpreterProgram.hpp
/// @brief Program-level state: loaded modules, IDs, instances, load/reload/run/teardown.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/memory/InternedString.hpp"
#include "InterpreterSession.hpp"
#include "ModuleRegistry.hpp"
#include "../support/ExecutionResult.hpp"

#include <vector>
#include <unordered_map>
#include <memory>
#include <string>

namespace interpreter {

/// @brief Represents a loaded program in the interpreter.
///
/// Owns the module instances, stable module IDs, ResourceTrackers,
/// and handles atomic load, reload, execution, and teardown.
///
/// ─── Session Lifetime ────────────────────────────────────────────────
/// An InterpreterProgram does NOT own its InterpreterSession and does
/// NOT hold a reference to it. Every method that needs the session
/// receives it as a parameter. The owner (Interpreter or
/// InterpreterContext) is responsible for calling teardown(session)
/// before the session is destroyed — see the destructor note below.
///
/// This is deliberate: the session is a long-lived host that outlives
/// any single program (load, reload, unload programs within one
/// session). Co-ownership via shared_ptr would be semantically wrong;
/// a stored reference would be a dangling-reference hazard the moment
/// a caller kept the program past the session's scope. Passing the
/// session explicitly makes the dependency visible at every call site
/// and forces the owner to sequence teardown before destruction.
class InterpreterProgram {
public:
    struct LoadedModule {
        ModuleAST* ast = nullptr;
        InternedString jitName;
        uint32_t id = UINT32_MAX;
        uint64_t instanceSize = 0;
        void* instance = nullptr;
        llvm::orc::ResourceTrackerSP tracker;
    };

    /// @brief Load a program from scratch. Fails atomically.
    /// @param session The interpreter session.
    /// @param modules The AST modules to load.
    /// @return The loaded program, or nullptr on failure.
    static std::unique_ptr<InterpreterProgram> load(
        InterpreterSession& session,
        const std::vector<ModuleAST*>& modules
    );

    /// @brief Destructor. Does NOT tear down — the owner must call
    ///        teardown(session) before destroying the program, while
    ///        the session is still alive. In debug builds, asserts that
    ///        teardown was called.
    ~InterpreterProgram();

    // Non-copyable
    InterpreterProgram(const InterpreterProgram&) = delete;
    InterpreterProgram& operator=(const InterpreterProgram&) = delete;

    /// @brief Hot-reload one or more modules in place.
    /// @param session The interpreter session.
    /// @param modules The modules to reload.
    /// @return true if reload succeeded.
    bool reload(
        InterpreterSession& session,
        const std::vector<ModuleAST*>& modules
    );

    /// @brief Execute the program's entry point.
    /// @param session The interpreter session.
    /// @param entryPoint The entry point function name (defaults to "main").
    /// @return The execution result.
    ExecutionResult run(
        InterpreterSession& session,
        InternedString entryPoint = InternedString()
    );

    /// @brief Tear down all owned resources (instances, JIT modules, registry).
    ///
    /// Must be called by the owner before the session is destroyed and
    /// before this program is destroyed. After this call the program is
    /// empty and must not be run.
    ///
    /// Idempotent: calling it twice is a no-op the second time.
    void teardown(InterpreterSession& session);

    /// @brief Get all loaded module ASTs.
    const std::vector<ModuleAST*>& modules() const { return m_moduleAsts; }

    /// @brief Get module AST for a given module ID.
    ModuleAST* moduleForId(uint32_t id) const;

    /// @brief Find module AST by file path.
    ModuleAST* findByPath(const std::string& path) const;

    /// @brief Get loaded module record by ID.
    const LoadedModule* getLoadedModule(uint32_t id) const;

    /// @brief Get the module registry.
    ModuleRegistry& registry() { return m_registry; }
    const ModuleRegistry& registry() const { return m_registry; }

private:
    explicit InterpreterProgram(InterpreterSession& session);

    // Free a single loaded module
    void unloadOne(InterpreterSession& session, LoadedModule& entry);

    // Helper to assign or query stable module ID
    uint32_t assignId(ModuleAST* module, const std::string& path);

    // Helper to generate a unique module name from an AST
    InternedString generateModuleName(StringPool& pool, ModuleAST* module);

    // Find entry point in loaded modules. Returns the FuncDeclAST whose
    // mangledName is the symbol to look up, or nullptr if not found.
    FuncDeclAST* findEntryPoint(InterpreterSession& session, InternedString entryPoint);

    std::vector<LoadedModule> m_modules;
    std::vector<ModuleAST*> m_moduleAsts;
    std::unordered_map<std::string, uint32_t> m_idByPath;
    std::unordered_map<ModuleAST*, uint32_t> m_idByAst;
    ModuleRegistry m_registry;
    uint32_t m_nextId = 0;
};

} // namespace interpreter