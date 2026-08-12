/// @file core/InterpreterContext.hpp
/// @brief Interpreter context holding shared state.

#pragma once

#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "../support/InterpreterOptions.hpp"
#include "../support/PanicHandler.hpp"
#include "../jit/JITSession.hpp"
#include "../dynlink/DynamicLinker.hpp"

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
    // Simple map of module name → AST (just for tracking what's loaded)
    std::unordered_map<uint32_t, ModuleAST*> loadedModules;
    bool hasActiveModule = false;
    InternedString activeModuleName;

    // ─── Constructor ────────────────────────────────────────────────────
    InterpreterContext(StringPool& p, DiagnosticEngine& d)
        : pool(p)
        , diagnostics(d)
        , jit(p) {}

    // Non-copyable
    InterpreterContext(const InterpreterContext&) = delete;
    InterpreterContext& operator=(const InterpreterContext&) = delete;
};

} // namespace interpreter