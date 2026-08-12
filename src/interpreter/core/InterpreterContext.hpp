/// @file core/InterpreterContext.hpp
/// @brief Interpreter context holding all shared state.

#pragma once

#include "ModuleRegistry.hpp"
#include "../support/InterpreterOptions.hpp"
#include "../support/PanicHandler.hpp"
#include "../dynlink/DynamicLinker.hpp"
#include "../jit/JITSession.hpp"
#include "../jit/JITCompiler.hpp"

#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"

namespace interpreter {

/// @brief Central context for the interpreter.
///
/// Holds all shared state and dependencies.
struct InterpreterContext {
    // ─── Resources ──────────────────────────────────────────────────────
    StringPool& pool;
    DiagnosticEngine& diagnostics;

    // ─── Components ────────────────────────────────────────────────────
    InterpreterOptions options;
    PanicHandler panicHandler;
    DynamicLinker linker;
    JITSession jit;
    JITCompiler compiler;
    ModuleRegistry modules;

    // ─── Constructor ────────────────────────────────────────────────────
    InterpreterContext(StringPool& p, DiagnosticEngine& d, const InterpreterOptions& opts = {})
        : pool(p)
        , diagnostics(d)
        , options(opts)
        , jit(p)
        , compiler(jit)
        , modules(p) {}

    // ─── State ─────────────────────────────────────────────────────────
    bool initialized = false;
    bool hasActiveModule = false;
    InternedString activeModuleName;
};

} // namespace interpreter