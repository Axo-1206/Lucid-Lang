/// @file execution/ModuleLoader.cpp
/// @brief Implementation of module loading functions.

#include "ModuleLoader.hpp"
#include "../support/InterpreterError.hpp"
#include "codegen/CodeGen.hpp"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <iostream>

namespace interpreter {

// ─── Module Loading ──────────────────────────────────────────────────────

bool loadModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Cannot load null module");
    }
    return loadModules(ctx, std::vector<ModuleAST*>{module});
}

bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    if (!ctx.jit.isInitialized()) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "JIT not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot load empty module list");
    }

    // ─── 1. Validate modules ──────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
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
    registerLibraries(ctx, modules);

    // ─── 3. Generate module name ─────────────────────────────────────────
    InternedString moduleName = generateModuleName(ctx, modules[0]);

    // ─── 4. Lower modules to LLVM IR ────────────────────────────────────
    auto irModule = lowerModules(ctx, modules, moduleName);
    if (!irModule) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Failed to lower modules to LLVM IR");
    }

    // ─── 5. Add to JIT ──────────────────────────────────────────────────
    ctx.jit.addModule(std::move(irModule), moduleName);

    // ─── 6. Track loaded modules ─────────────────────────────────────────
    for (ModuleAST* module : modules) {
        ctx.loadedModules[moduleName.id] = module;
    }
    ctx.hasActiveModule = true;
    ctx.activeModuleName = moduleName;

    if (ctx.options.verbose) {
        std::cout << "Loaded module: " << ctx.pool.lookup(moduleName) << "\n";
    }

    return true;
}

bool isModuleLoaded(const InterpreterContext& ctx, InternedString name) {
    return ctx.loadedModules.find(name.id) != ctx.loadedModules.end();
}

ModuleAST* getActiveModule(const InterpreterContext& ctx) {
    if (!ctx.hasActiveModule || !ctx.activeModuleName.isValid()) {
        return nullptr;
    }
    auto it = ctx.loadedModules.find(ctx.activeModuleName.id);
    return it != ctx.loadedModules.end() ? it->second : nullptr;
}

// ─── Internal Helpers ────────────────────────────────────────────────────

InternedString generateModuleName(InterpreterContext& ctx, ModuleAST* module) {
    if (!module || !module->filePath.isValid()) {
        // Generate a unique name based on pointer
        std::string name = "module_" + std::to_string(reinterpret_cast<uintptr_t>(module));
        return ctx.pool.intern(name);
    }

    std::string name = ctx.pool.lookup(module->filePath);
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    
    return ctx.pool.intern(name);
}

std::unique_ptr<llvm::Module> lowerModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        return nullptr;
    }

    // ─── Use the CodeGen module to generate IR ──────────────────────────
    // This is a temporary implementation - we'll use the existing codegen
    llvm::LLVMContext llvmCtx;
    auto modules = codegen::generate({module}, llvmCtx);
    
    if (modules.empty() || !modules[0]) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, module,
                              "failed to generate IR for module '",
                              ctx.pool.lookup(module->filePath), "'");
        return nullptr;
    }

    return std::move(modules[0]);
}

std::unique_ptr<llvm::Module> lowerModules(
    InterpreterContext& ctx,
    const std::vector<ModuleAST*>& modules,
    InternedString moduleName
) {
    if (modules.empty()) {
        return nullptr;
    }

    // ─── For multiple modules, we combine them into one LLVM module ────
    // This is a simplified implementation
    
    // If only one module, just lower it
    if (modules.size() == 1) {
        return lowerModule(ctx, modules[0]);
    }

    // For multiple modules, we need to combine them
    // TODO: Implement proper multi-module lowering
    ctx.diagnostics.warning(DiagCode::Warn_UnreachableCode, nullptr,
                            "multi-module lowering is not fully implemented, "
                            "only the first module will be used");

    return lowerModule(ctx, modules[0]);
}

bool hasErrors(const std::vector<ModuleAST*>& modules) {
    for (const ModuleAST* module : modules) {
        if (module && module->hasErrors) {
            return true;
        }
    }
    return false;
}

void reportErrors(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    for (const ModuleAST* module : modules) {
        if (module && module->hasErrors) {
            // The module's errors are already in the diagnostic engine
            // We just need to ensure they are displayed
            ctx.diagnostics.dump(std::cerr);
        }
    }
}

} // namespace interpreter