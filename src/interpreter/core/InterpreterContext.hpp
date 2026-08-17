/// @file core/InterpreterContext.hpp
/// @brief Interpreter context holding shared state.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "ModuleRegistry.hpp"
#include "../support/InterpreterOptions.hpp"
#include "../support/PanicHandler.hpp"
#include "../jit/JITSession.hpp"
#include "../dynlink/DynamicLinker.hpp"

#include <unordered_map>
#include <memory>

namespace interpreter {

/// @brief Central context for the interpreter.
///
/// Holds shared state and dependencies, following the same pattern
/// as SemaContext and CodeGenContext. Contains no behavior - just state.
struct InterpreterContext {
    // ─── Resources ──────────────────────────────────────────────────────
    StringPool& pool;
    DiagnosticEngine& diagnostics;

    // ─── State ─────────────────────────────────────────────────────────
    InterpreterOptions options;
    PanicHandler panicHandler;
    DynamicLinker linker;
    JITSession jit;

    // ─── Module Tracking ──────────────────────────────────────────────
    // Registry for tracking loaded modules and their versions
    ModuleRegistry moduleRegistry;

    // ─── Constructor ────────────────────────────────────────────────────
    InterpreterContext(StringPool& p, DiagnosticEngine& d)
        : pool(p)
        , diagnostics(d)
        , jit(p)
        , moduleRegistry(p) {}

    // Non-copyable
    InterpreterContext(const InterpreterContext&) = delete;
    InterpreterContext& operator=(const InterpreterContext&) = delete;

    // ─── Convenience Accessors ────────────────────────────────────────

    /// @brief Get the active module.
    ModuleInfo* getActiveModule() {
        return moduleRegistry.getActiveModule();
    }

    /// @brief Get the active module (const).
    const ModuleInfo* getActiveModule() const {
        return moduleRegistry.getActiveModule();
    }

    /// @brief Check if a module is loaded.
    bool hasModule(InternedString name) const {
        return moduleRegistry.hasModule(name);
    }

    /// @brief Get module info by name.
    ModuleInfo* getModuleInfo(InternedString name) {
        return moduleRegistry.getModuleInfo(name);
    }

    /// @brief Get module info by name (const).
    const ModuleInfo* getModuleInfo(InternedString name) const {
        return moduleRegistry.getModuleInfo(name);
    }
};

} // namespace interpreter