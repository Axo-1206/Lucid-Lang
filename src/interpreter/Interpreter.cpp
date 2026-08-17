/// @file Interpreter.cpp
/// @brief Implementation of the main interpreter API.

#include "Interpreter.hpp"
#include "support/InterpreterError.hpp"
#include "execution/ModuleLoader.hpp"
#include "execution/SymbolResolver.hpp"
#include "dynlink/DynamicLinker.hpp"

#include <chrono>
#include <iostream>

namespace interpreter {

// ─── Initialization ──────────────────────────────────────────────────────

void initialize(InterpreterContext& ctx, const InterpreterOptions& options) {
    if (ctx.jit.isInitialized()) {
        return;
    }

    ctx.options = options;

    try {
        ctx.jit.initialize();

        if (options.verbose) {
            std::cout << "Interpreter initialized successfully\n";
            std::cout << "  Optimization level: " << options.optimizationLevel << "\n";
            std::cout << "  Debug info: " << (options.enableDebugInfo ? "enabled" : "disabled") << "\n";
            std::cout << "  Hot-reload: " << (options.enableHotReload ? "enabled" : "disabled") << "\n";
        }
    } catch (const std::exception& e) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                              "interpreter initialization failed: ", e.what());
        throw InterpreterError(InterpreterErrorKind::InitFailed, e.what());
    }
}

bool isInitialized(const InterpreterContext& ctx) {
    return ctx.jit.isInitialized();
}

// ─── Execution ──────────────────────────────────────────────────────────

ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module,
                          InternedString entryPoint, bool isHotReload) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "Cannot run null module");
    }
    return runModules(ctx, std::vector<ModuleAST*>{module}, entryPoint, isHotReload);
}

ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module,
                          const std::string& entryPoint, bool isHotReload) {
    InternedString entryPointInterned = entryPoint.empty() 
        ? InternedString() 
        : ctx.pool.intern(entryPoint);
    return runModule(ctx, module, entryPointInterned, isHotReload);
}

ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint,
                           bool isHotReload) {
    if (!ctx.jit.isInitialized()) {
        throw InterpreterError(InterpreterErrorKind::InitFailed,
                               "Interpreter not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "Cannot run empty module list");
    }

    // ─── Validate modules ──────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                                   "Cannot run null module in list");
        }
        if (module->hasErrors) {
            ctx.diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                                  "module '", ctx.pool.lookup(module->filePath),
                                  "' has semantic errors");
            return ExecutionResult{1, false, "Module has semantic errors"};
        }
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    try {
        // ─── 1. Load or reload modules ─────────────────────────────────
        // Delegate to ModuleLoader
        if (!loadOrReloadModules(ctx, modules, isHotReload)) {
            return ExecutionResult{1, false, "Failed to load modules"};
        }

        // ─── 2. Determine entry point ──────────────────────────────────
        // Delegate to SymbolResolver
        if (!entryPoint.isValid()) {
            entryPoint = ctx.pool.intern("main");
        }

        InternedString foundEntry = findEntryPoint(ctx, entryPoint);
        
        if (!foundEntry.isValid()) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                                  "entry point '", ctx.pool.lookup(entryPoint), 
                                  "' not found");
            throw InterpreterError(InterpreterErrorKind::EntryPointNotFound,
                                   "Entry point not found: " + 
                                   ctx.pool.lookup(entryPoint));
        }

        // ─── 3. Execute entry point ────────────────────────────────────
        // Delegate to JITSession for symbol lookup
        std::string foundName = ctx.pool.lookup(foundEntry);
        void* fnPtr = ctx.jit.lookupSymbol(foundName);
        if (!fnPtr) {
            ctx.diagnostics.error(DiagCode::Ffi_UnknownSymbol, nullptr,
                                  "symbol '", foundName, "' not found in JIT");
            throw InterpreterError(InterpreterErrorKind::SymbolLookupFailed,
                                   "Symbol lookup failed: " + foundName);
        }

        int exitCode = 0;
        try {
            auto mainFn = reinterpret_cast<int(*)()>(fnPtr);
            exitCode = mainFn();
        } catch (const std::exception& e) {
            exitCode = ctx.panicHandler.handle(e);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime);

        ExecutionResult result;
        result.exitCode = exitCode;
        result.success = true;
        result.executionTimeMs = duration.count() / 1000.0;
        result.entryPointUsed = foundName;

        if (ctx.options.verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const InterpreterError& e) {
        // Error was already reported via DiagnosticEngine
        throw;
    } catch (const std::exception& e) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                              "execution failed: ", e.what());
        throw InterpreterError(InterpreterErrorKind::ExecutionFailed, e.what());
    }
}

ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           const std::string& entryPoint,
                           bool isHotReload) {
    InternedString entryPointInterned = entryPoint.empty() 
        ? InternedString() 
        : ctx.pool.intern(entryPoint);
    return runModules(ctx, modules, entryPointInterned, isHotReload);
}

// ─── Hot-Reload ─────────────────────────────────────────────────────────

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     InternedString name) {
    // Delegate to ModuleLoader
    return ::interpreter::hotReloadModule(ctx, module, name);
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     const std::string& name) {
    // Delegate to ModuleLoader
    return ::interpreter::hotReloadModule(ctx, module, name);
}

// ─── Convenience Accessors ─────────────────────────────────────────────

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx) {
    // Delegate to ModuleRegistry
    return ctx.moduleRegistry.getAllModules();
}

std::vector<ModuleInfo*> getAffectedModules(InterpreterContext& ctx, 
                                            InternedString changedModule) {
    // Delegate to ModuleRegistry
    return ctx.moduleRegistry.getAffectedModules(changedModule);
}

} // namespace interpreter