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
#include <vector>

namespace interpreter {

/// @brief Central context for the interpreter.
struct InterpreterContext {
    // ─── Resources ──────────────────────────────────────────────────────
    StringPool& pool;
    DiagnosticEngine& diagnostics;

    // ─── State ─────────────────────────────────────────────────────────
    InterpreterOptions options;
    PanicHandler panicHandler;
    DynamicLinker linker;
    JITSession jit;
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

    ModuleInfo* getActiveModule() {
        return moduleRegistry.getActiveModule();
    }

    const ModuleInfo* getActiveModule() const {
        return moduleRegistry.getActiveModule();
    }

    bool hasModule(InternedString name) const {
        return moduleRegistry.hasModule(name);
    }

    ModuleInfo* getModuleInfo(InternedString name) {
        return moduleRegistry.getModuleInfo(name);
    }

    const ModuleInfo* getModuleInfo(InternedString name) const {
        return moduleRegistry.getModuleInfo(name);
    }
    
    /// @brief Check if any modules have semantic errors.
    bool hasErrorModules() const {
        return moduleRegistry.hasErrorModules();
    }
};

} // namespace interpreter