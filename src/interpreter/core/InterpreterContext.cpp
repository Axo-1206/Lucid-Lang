/// @file Interpreter.cpp
/// @brief Implementation of the main interpreter API.

#include "../Interpreter.hpp"
#include "../support/InterpreterError.hpp"
#include "../execution/SymbolResolver.hpp"

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
        throw InterpreterError(InterpreterError::Kind::InitFailed, e.what());
    }
}

bool isInitialized(const InterpreterContext& ctx) {
    return ctx.jit.isInitialized();
}

// ─── Execution ──────────────────────────────────────────────────────────

ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module,
                          const std::string& entryPoint) {
    if (!module) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot run null module");
    }
    return runModules(ctx, std::vector<ModuleAST*>{module}, entryPoint);
}

ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           const std::string& entryPoint) {
    if (!ctx.jit.isInitialized()) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot run empty module list");
    }

    // ─── Validate modules ──────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
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
        // 1. Register foreign libraries
        registerLibraries(ctx, modules);

        // 2. Load modules
        if (!loadModules(ctx, modules)) {
            return ExecutionResult{1, false, "Failed to load modules"};
        }

        // 3. Find entry point
        InternedString entryPointInterned = ctx.pool.intern(entryPoint);
        InternedString foundEntry = findEntryPoint(ctx, entryPointInterned);
        
        if (!foundEntry.isValid()) {
            reportEntryPointNotFound(ctx.diagnostics, entryPoint);
            return ExecutionResult{1, false, "Entry point not found: " + entryPoint};
        }

        // 4. Execute entry point
        void* fnPtr = lookupSymbol(ctx, ctx.pool.lookup(foundEntry));
        if (!fnPtr) {
            reportSymbolLookupError(ctx.diagnostics,
                                   ctx.pool.lookup(foundEntry),
                                   "symbol not found in JIT");
            return ExecutionResult{1, false, "Symbol lookup failed"};
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
        result.entryPointUsed = entryPoint;

        if (ctx.options.verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const InterpreterError& e) {
        if (e.hasCode()) {
            e.report(ctx.diagnostics);
        }
        throw;
    } catch (const std::exception& e) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                              "execution failed: ", e.what());
        throw InterpreterError(InterpreterError::Kind::ExecutionFailed, e.what());
    }
}

// ─── Hot-Reload ─────────────────────────────────────────────────────────

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name) {
    if (!ctx.jit.isInitialized()) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "JIT not initialized");
    }

    if (!module) {
        throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                               "Cannot reload null module");
    }

    if (module->hasErrors) {
        ctx.diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                              "module '", ctx.pool.lookup(module->filePath),
                              "' has semantic errors");
        return false;
    }

    InternedString moduleName = ctx.pool.intern(name);

    if (!ctx.options.enableHotReload) {
        throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                               "Hot-reload is not enabled");
    }

    try {
        // 1. Register new foreign libraries
        registerLibraries(ctx, module);

        // 2. Generate versioned name
        static uint64_t versionCounter = 0;
        std::string versionedName = name + "_v" + std::to_string(++versionCounter);
        InternedString versionedNameInterned = ctx.pool.intern(versionedName);

        // 3. Lower the module
        auto irModule = lowerModule(ctx, module);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                                   "Failed to lower module to LLVM IR");
        }

        // 4. Add new version to JIT
        ctx.jit.addModule(std::move(irModule), versionedNameInterned);

        // 5. Remove old version
        if (ctx.jit.hasModule(moduleName)) {
            ctx.jit.removeModule(moduleName);
        }

        // 6. Update tracking
        ctx.loadedModules.erase(moduleName.id);
        ctx.loadedModules[versionedNameInterned.id] = module;
        ctx.activeModuleName = versionedNameInterned;

        if (ctx.options.verbose) {
            std::cout << "Hot-reload successful: " << name << " -> " << versionedName << "\n";
        }

        return true;

    } catch (const std::exception& e) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                              "hot-reload failed: ", e.what());
        throw InterpreterError(InterpreterError::Kind::HotReloadFailed, e.what());
    }
}

// ─── Foreign Libraries ──────────────────────────────────────────────────

void registerLibrary(InterpreterContext& ctx, const std::string& name) {
    try {
        if (ctx.linker.isLoaded(name)) {
            return;
        }
        ctx.linker.load(name);
        ctx.linker.registerWithJIT(ctx.jit);
    } catch (const std::exception& e) {
        reportLibraryLoadError(ctx.diagnostics, name, e.what());
        throw InterpreterError(InterpreterError::Kind::LibraryLoadFailed, e.what());
    }
}

void registerLibraries(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) return;
    registerLibraries(ctx, std::vector<ModuleAST*>{module});
}

void registerLibrariesFromModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) return;

    InternedString linkName = ctx.pool.intern("link");
    for (DeclPtr decl : module->decls) {
        for (AttributePtr attr : decl->attributes) {
            if (attr->name == linkName) {
                for (LiteralExprAST* arg : attr->args) {
                    if (arg->kind == LiteralKind::String || 
                        arg->kind == LiteralKind::RawString) {
                        std::string libName = ctx.pool.lookup(arg->value);
                        bool isPath = libName.find('/') != std::string::npos ||
                                      libName.find('\\') != std::string::npos ||
                                      libName.find('.') != std::string::npos;
                        if (!isPath) {
                            try {
                                registerLibrary(ctx, libName);
                            } catch (const std::exception& e) {
                                if (ctx.options.verbose) {
                                    std::cerr << "Warning: " << e.what() << "\n";
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void registerLibraries(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    for (ModuleAST* module : modules) {
        registerLibrariesFromModule(ctx, module);
    }
}

// ─── Symbol Lookup ──────────────────────────────────────────────────────

void* lookupSymbol(InterpreterContext& ctx, const std::string& name) {
    if (!ctx.jit.isInitialized()) {
        return nullptr;
    }
    try {
        return ctx.jit.lookupSymbol(name);
    } catch (const std::exception& e) {
        reportSymbolLookupError(ctx.diagnostics, name, e.what());
        return nullptr;
    }
}

} // namespace interpreter