/// @file core/InterpreterContext.hpp
/// @brief Compatibility interpreter context holding shared state.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/ast/BaseAST.hpp"
#include "InterpreterSession.hpp"
#include "InterpreterProgram.hpp"
#include "../support/InterpreterOptions.hpp"
#include "../support/PanicHandler.hpp"
#include "../jit/JITSession.hpp"
#include "../dynlink/DynamicLinker.hpp"

#include <memory>
#include <vector>

namespace interpreter {

/// @brief Compatibility context for the interpreter.
struct InterpreterContext {
    StringPool& pool;
    DiagnosticEngine& diagnostics;
    InterpreterOptions options;
    InterpreterSession session;
    std::unique_ptr<InterpreterProgram> program;

    InterpreterContext(StringPool& p, DiagnosticEngine& d)
        : pool(p)
        , diagnostics(d)
        , session(p, d) {}

    // Non-copyable
    InterpreterContext(const InterpreterContext&) = delete;
    InterpreterContext& operator=(const InterpreterContext&) = delete;

    // ─── Convenience Accessors ────────────────────────────────────────

    JITSession& jit() { return session.jit(); }
    DynamicLinker& linker() { return session.linker(); }
    PanicHandler& panicHandler() { return session.panicHandler(); }

    ModuleRegistry* getModuleRegistry() {
        return program ? &program->registry() : nullptr;
    }

    ModuleInfo* getActiveModule() {
        return program ? program->registry().getActiveModule() : nullptr;
    }

    const ModuleInfo* getActiveModule() const {
        return program ? program->registry().getActiveModule() : nullptr;
    }

    bool hasModule(InternedString name) const {
        return program ? program->registry().hasModule(name) : false;
    }

    ModuleInfo* getModuleInfo(InternedString name) {
        return program ? program->registry().getModuleInfo(name) : nullptr;
    }

    const ModuleInfo* getModuleInfo(InternedString name) const {
        return program ? program->registry().getModuleInfo(name) : nullptr;
    }

    bool hasErrorModules() const {
        return program ? program->registry().hasErrorModules() : false;
    }
};

} // namespace interpreter