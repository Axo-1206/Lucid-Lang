/// @file execution/ModuleLoader.cpp
/// @brief Implementation of module loading functions.

#include "ModuleLoader.hpp"
#include "../support/InterpreterError.hpp"
#include "codegen/CodeGen.hpp"
#include "../dynlink/DynamicLinker.hpp"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <iostream>
#include <queue>
#include <set>

namespace interpreter {

// ─── Core Loading Functions ─────────────────────────────────────────────

bool loadOrReloadModules(InterpreterContext& ctx, 
                         const std::vector<ModuleAST*>& modules,
                         bool isHotReload) {
    if (!ctx.jit.isInitialized()) {
        throw InterpreterError(InterpreterErrorKind::InitFailed,
                               "JIT not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "Cannot load empty module list");
    }

    // ─── 1. Validate modules ──────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                                   "Cannot load null module in list");
        }
        if (module->hasErrors) {
            ctx.diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                                  "module '", ctx.pool.lookup(module->filePath),
                                  "' has semantic errors");
            return false;
        }
    }

    // ─── 2. Register foreign libraries ──────────────────────────────────
    registerModuleLibraries(ctx, modules);

    // ─── 3. Generate module names and extract dependencies ──────────────
    std::vector<InternedString> moduleNames;
    moduleNames.reserve(modules.size());
    
    std::vector<std::vector<InternedString>> moduleDeps;
    moduleDeps.reserve(modules.size());

    for (ModuleAST* module : modules) {
        InternedString name = generateModuleName(ctx, module);
        moduleNames.push_back(name);
        moduleDeps.push_back(extractModuleDependencies(ctx, module));
    }

    // ─── 4. Lower modules to LLVM IR ────────────────────────────────────
    auto irModules = lowerModulesSeparately(ctx, modules);
    if (irModules.size() != modules.size()) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Failed to lower all modules to LLVM IR");
    }

    // ─── 5. Load or reload modules ──────────────────────────────────────
    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleAST* module = modules[i];
        InternedString name = moduleNames[i];
        auto& irModule = irModules[i];

        if (isHotReload) {
            // ─── Hot Reload Path ──────────────────────────────────────────
            // Remove old version if it exists (same name)
            if (ctx.jit.hasModule(name)) {
                ctx.jit.removeModule(name);
            }
            
            // Add new version (same name, replaces old)
            ctx.jit.addModule(std::move(irModule), name);
            
            // Update registry (overwrites old AST)
            ctx.moduleRegistry.registerModule(name, module);
            ctx.moduleRegistry.setDependencies(name, moduleDeps[i]);

            if (ctx.options.verbose) {
                std::cout << "Hot-reloaded module: " << ctx.pool.lookup(name) << "\n";
            }
        } else {
            // ─── Initial Load Path ────────────────────────────────────────
            ctx.jit.addModule(std::move(irModule), name);

            ctx.moduleRegistry.registerModule(name, module);
            ctx.moduleRegistry.setDependencies(name, moduleDeps[i]);

            if (ctx.options.verbose) {
                std::cout << "Loaded module: " << ctx.pool.lookup(name) << "\n";
            }
        }
    }

    // ─── 6. Set active module ────────────────────────────────────────────
    ctx.moduleRegistry.setActiveModule(moduleNames[0]);

    return true;
}

// ─── Convenience Wrappers ──────────────────────────────────────────────

bool loadModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Cannot load null module");
    }
    return loadModules(ctx, std::vector<ModuleAST*>{module});
}

bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    return loadOrReloadModules(ctx, modules, false);
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     InternedString name) {
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

    // Get all affected modules (dependents)
    std::vector<ModuleInfo*> affectedModules = getAffectedModules(ctx, name);
    
    // Build list of modules to reload (changed + dependents)
    std::vector<ModuleAST*> modulesToReload;
    modulesToReload.push_back(module);
    
    for (ModuleInfo* info : affectedModules) {
        if (info && info->ast) {
            modulesToReload.push_back(info->ast);
        }
    }

    if (ctx.options.verbose) {
        std::cout << "Hot-reloading " << modulesToReload.size() 
                  << " module(s) (changed + dependents)\n";
    }

    return loadOrReloadModules(ctx, modulesToReload, true);
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, 
                     const std::string& name) {
    InternedString nameInterned = ctx.pool.intern(name);
    return hotReloadModule(ctx, module, nameInterned);
}

bool isModuleLoaded(const InterpreterContext& ctx, InternedString name) {
    return ctx.moduleRegistry.hasModule(name);
}

ModuleAST* getActiveModule(const InterpreterContext& ctx) {
    const ModuleInfo* info = ctx.moduleRegistry.getActiveModule();
    return info ? info->ast : nullptr;
}

std::vector<ModuleInfo*> getAffectedModules(InterpreterContext& ctx, 
                                            InternedString changedModule) {
    return ctx.moduleRegistry.getAffectedModules(changedModule);
}

// ─── Internal Helpers ────────────────────────────────────────────────────

InternedString generateModuleName(InterpreterContext& ctx, ModuleAST* module) {
    if (!module || !module->filePath.isValid()) {
        std::string name = "module_" + 
                          std::to_string(reinterpret_cast<uintptr_t>(module));
        return ctx.pool.intern(name);
    }

    std::string name = ctx.pool.lookup(module->filePath);
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    
    return ctx.pool.intern(name);
}

std::unique_ptr<llvm::Module> lowerModule(InterpreterContext& ctx, 
                                          ModuleAST* module) {
    if (!module) {
        return nullptr;
    }

    auto generatedModules = codegen::generate(
        std::vector<ModuleAST*>{module},
        ctx.pool,
        ctx.diagnostics,
        ctx.jit.getContext()
    );
    
    if (generatedModules.empty() || !generatedModules[0]) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, module,
                              "failed to generate IR for module '",
                              ctx.pool.lookup(module->filePath), "'");
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Failed to generate IR for module: " + 
                               ctx.pool.lookup(module->filePath));
    }

    return std::move(generatedModules[0]);
}

std::vector<std::unique_ptr<llvm::Module>> lowerModulesSeparately(
    InterpreterContext& ctx,
    const std::vector<ModuleAST*>& modules) {
    
    std::vector<std::unique_ptr<llvm::Module>> result;
    result.reserve(modules.size());

    for (ModuleAST* module : modules) {
        auto irModule = lowerModule(ctx, module);
        if (!irModule) {
            throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                                   "Failed to lower module: " + 
                                   ctx.pool.lookup(module->filePath));
        }
        result.push_back(std::move(irModule));
    }

    return result;
}

std::vector<InternedString> extractModuleDependencies(
    InterpreterContext& ctx,
    ModuleAST* module) {
    
    std::vector<InternedString> dependencies;
    
    if (!module) {
        return dependencies;
    }

    // Look for import declarations in the module
    // This is a placeholder - actual implementation depends on
    // how imports are stored in your AST
    
    return dependencies;
}

void registerModuleLibraries(InterpreterContext& ctx, 
                            const std::vector<ModuleAST*>& modules) {
    // Delegate to DynamicLinker with explicit dependencies
    ctx.linker.registerLibrariesFromModules(
        ctx.diagnostics,
        ctx.pool,
        ctx.options.verbose,
        modules
    );
}

} // namespace interpreter