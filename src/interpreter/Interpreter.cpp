/// @file Interpreter.cpp
/// @brief Implementation of the main interpreter API.

#include "Interpreter.hpp"
#include "support/InterpreterError.hpp"
#include "execution/SymbolResolver.hpp"

#include <chrono>
#include <iostream>

namespace interpreter {

// ─── Forward Declarations ──────────────────────────────────────────────

static std::unique_ptr<llvm::Module> lowerModule(InterpreterContext& ctx, ModuleAST* module);
static InternedString findEntryPoint(InterpreterContext& ctx, InternedString entryPoint);

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
                          InternedString entryPoint) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "Cannot run null module");
    }
    return runModules(ctx, std::vector<ModuleAST*>{module}, entryPoint);
}

ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module,
                          const std::string& entryPoint) {
    InternedString entryPointInterned = entryPoint.empty() 
        ? InternedString() 
        : ctx.pool.intern(entryPoint);
    return runModule(ctx, module, entryPointInterned);
}

ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint) {
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
        // 1. Register foreign libraries
        registerLibraries(ctx, modules);

        // 2. Load modules
        if (!loadModules(ctx, modules)) {
            return ExecutionResult{1, false, "Failed to load modules"};
        }

        // 3. Determine entry point
        InternedString actualEntryPoint = entryPoint;
        if (!actualEntryPoint.isValid()) {
            actualEntryPoint = ctx.pool.intern("main");
        }

        InternedString foundEntry = findEntryPoint(ctx, actualEntryPoint);
        
        if (!foundEntry.isValid()) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, nullptr,
                                  "entry point '", ctx.pool.lookup(actualEntryPoint), 
                                  "' not found");
            throw InterpreterError(InterpreterErrorKind::EntryPointNotFound,
                                   "Entry point not found: " + ctx.pool.lookup(actualEntryPoint));
        }

        // 4. Execute entry point
        std::string foundName = ctx.pool.lookup(foundEntry);
        void* fnPtr = lookupSymbol(ctx, foundName);
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
                           const std::string& entryPoint) {
    InternedString entryPointInterned = entryPoint.empty() 
        ? InternedString() 
        : ctx.pool.intern(entryPoint);
    return runModules(ctx, modules, entryPointInterned);
}

// ─── Loading ────────────────────────────────────────────────────────────

bool loadModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Cannot load null module");
    }

    if (module->hasErrors) {
        ctx.diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                              "module '", ctx.pool.lookup(module->filePath),
                              "' has semantic errors");
        return false;
    }

    // Register foreign libraries
    registerLibraries(ctx, module);

    // Generate module name from file path
    std::string filePath = ctx.pool.lookup(module->filePath);
    std::string moduleName = filePath;
    
    // Remove path and extension
    size_t lastSlash = moduleName.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        moduleName = moduleName.substr(lastSlash + 1);
    }
    size_t lastDot = moduleName.find_last_of('.');
    if (lastDot != std::string::npos) {
        moduleName = moduleName.substr(0, lastDot);
    }

    InternedString name = ctx.pool.intern(moduleName);

    // Lower to IR and compile
    auto irModule = lowerModule(ctx, module);
    if (!irModule) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, module,
                              "failed to lower module '", moduleName, "' to IR");
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Failed to lower module to LLVM IR");
    }

    ctx.jit.addModule(std::move(irModule), name);
    
    // Register the module
    ctx.moduleRegistry.registerModule(name, module);
    ctx.moduleRegistry.setActiveModule(name);

    if (ctx.options.verbose) {
        std::cout << "Loaded module: " << moduleName << "\n";
    }

    return true;
}

bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    if (modules.empty()) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "Cannot load empty module list");
    }

    // Register libraries from all modules first
    registerLibraries(ctx, modules);

    // Load each module in order
    for (ModuleAST* module : modules) {
        if (!loadModule(ctx, module)) {
            return false;
        }
    }

    return true;
}

// ─── Hot-Reload ─────────────────────────────────────────────────────────

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, InternedString name) {
    if (!ctx.jit.isInitialized()) {
        throw InterpreterError(InterpreterErrorKind::InitFailed,
                               "JIT not initialized");
    }

    if (!module) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Cannot reload null module");
    }

    if (module->hasErrors) {
        ctx.diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                              "module '", ctx.pool.lookup(module->filePath),
                              "' has semantic errors");
        return false;
    }

    if (!ctx.options.enableHotReload) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Hot-reload is not enabled");
    }

    try {
        // 1. Register new foreign libraries
        registerLibraries(ctx, module);

        // 2. Increment version and generate versioned name
        uint64_t newVersion = ctx.moduleRegistry.incrementVersion(name);
        std::string nameStr = ctx.pool.lookup(name);
        std::string versionedName = nameStr + "_v" + std::to_string(newVersion);
        InternedString versionedNameInterned = ctx.pool.intern(versionedName);

        // 3. Lower the module
        auto irModule = lowerModule(ctx, module);
        if (!irModule) {
            ctx.diagnostics.error(DiagCode::Backend_CodegenError, module,
                                  "failed to lower module '", nameStr, "' for hot-reload");
            throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                                   "Failed to lower module to LLVM IR");
        }

        // 4. Add new version to JIT
        ctx.jit.addModule(std::move(irModule), versionedNameInterned);

        // 5. Remove old version
        if (ctx.jit.hasModule(name)) {
            ctx.jit.removeModule(name);
        }

        // 6. Update registry
        ctx.moduleRegistry.registerModule(versionedNameInterned, module);
        ctx.moduleRegistry.setActiveModule(versionedNameInterned);

        if (ctx.options.verbose) {
            std::cout << "Hot-reload successful: " << nameStr << " -> " << versionedName << "\n";
        }

        return true;

    } catch (const InterpreterError& e) {
        throw;
    } catch (const std::exception& e) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                              "hot-reload failed: ", e.what());
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed, e.what());
    }
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name) {
    InternedString nameInterned = ctx.pool.intern(name);
    return hotReloadModule(ctx, module, nameInterned);
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
        ctx.diagnostics.error(DiagCode::Ffi_LibraryNotFound, nullptr,
                              "failed to load library '", name, "': ", e.what());
        throw InterpreterError(InterpreterErrorKind::LibraryLoadFailed,
                               "Failed to load library: " + name);
    }
}

void registerLibrariesFromModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) return;

    InternedString linkName = ctx.pool.intern("link");
    for (DeclAST* decl : module->decls) {
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
                            } catch (const InterpreterError& e) {
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

void registerLibraries(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) return;
    registerLibraries(ctx, std::vector<ModuleAST*>{module});
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
        ctx.diagnostics.error(DiagCode::Ffi_UnknownSymbol, nullptr,
                              "symbol '", name, "' lookup failed: ", e.what());
        throw InterpreterError(InterpreterErrorKind::SymbolLookupFailed,
                               "Symbol lookup failed: " + name);
    }
}

void* lookupSymbol(InterpreterContext& ctx, InternedString name) {
    if (!name.isValid()) {
        return nullptr;
    }
    return lookupSymbol(ctx, ctx.pool.lookup(name));
}

// ─── Accessors ──────────────────────────────────────────────────────────

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx) {
    return ctx.moduleRegistry.getAllModules();
}

// ─── Helper Implementations ─────────────────────────────────────────────

static InternedString findEntryPoint(InterpreterContext& ctx, InternedString entryPoint) {
    if (!entryPoint.isValid()) {
        return InternedString();
    }

    // Check if the entry point exists in the JIT
    void* ptr = ctx.jit.lookupSymbol(entryPoint);
    if (ptr) {
        return entryPoint;
    }

    // Try the entry point as a string
    std::string name = ctx.pool.lookup(entryPoint);
    ptr = ctx.jit.lookupSymbol(name);
    if (ptr) {
        return entryPoint;
    }

    return InternedString();
}

static std::unique_ptr<llvm::Module> lowerModule(InterpreterContext& ctx, ModuleAST* module) {
    // This is a stub - actual implementation would use the code generation
    // pipeline to convert AST to LLVM IR.
    // In the real implementation, you'd call:
    // return codegen::generateIR(ctx, module);
    
    // Placeholder: create an empty module
    auto context = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("module", *context);
    return mod;
}

} // namespace interpreter