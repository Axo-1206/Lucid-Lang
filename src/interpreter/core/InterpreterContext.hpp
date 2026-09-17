/// @file core/InterpreterContext.hpp
/// @brief Compatibility interpreter context holding shared state.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/ast/BaseAST.hpp"
#include "InterpreterSession.hpp"
#include "InterpreterProgram.hpp"
#include "../support/InterpreterOptions.hpp"
#include "../jit/JITSession.hpp"
#include "../dynlink/DynamicLinker.hpp"

#include <memory>
#include <vector>

namespace interpreter {

/// @brief Compatibility context for the interpreter.
///
/// ─── Session and Program Lifetime ────────────────────────────────────
/// The context owns both an InterpreterSession (by value) and an
/// InterpreterProgram (by unique_ptr). session is declared first, so it
/// is destroyed last; the destructor body calls program->teardown(session)
/// while both are still alive, and then program's own destructor only
/// asserts that teardown happened.
///
/// This mirrors the Interpreter facade's lifetime discipline. See
/// InterpreterProgram's class doc for why the program does not hold a
/// reference to the session.
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

    ~InterpreterContext() {
        // Tear down the program while the session is still alive. Member
        // destruction order (session declared before program, destroyed
        // after) means both are valid in this destructor body.
        if (program) {
            program->teardown(session);
            program.reset();
        }
    }

    // Non-copyable
    InterpreterContext(const InterpreterContext&) = delete;
    InterpreterContext& operator=(const InterpreterContext&) = delete;

    // ─── Convenience Accessors ────────────────────────────────────────

    JITSession& jit() { return session.jit(); }
    DynamicLinker& linker() { return session.linker(); }

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