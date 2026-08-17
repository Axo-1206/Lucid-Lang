/// @file execution/ModuleLoader.cpp
/// @brief Implementation of module loading functions.

#include "ModuleLoader.hpp"
#include "../support/InterpreterError.hpp"
#include "codegen/CodeGen.hpp"
#include "interpreter/Interpreter.hpp"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <iostream>

namespace interpreter {

// ─── Module Loading ──────────────────────────────────────────────────────

bool loadModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Cannot load null module");
    }
    return loadModules(ctx, std::vector<ModuleAST*>{module});
}

bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
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

    // ─── 3. Generate module name ─────────────────────────────────────────
    InternedString moduleName = generateModuleName(ctx, modules[0]);

    // ─── 4. Lower modules to LLVM IR ────────────────────────────────────
    auto irModule = lowerModules(ctx, modules, moduleName);
    if (!irModule) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Failed to lower modules to LLVM IR");
    }

    // ─── 5. Add to JIT ──────────────────────────────────────────────────
    ctx.jit.addModule(std::move(irModule), moduleName);

    // ─── 6. Track loaded modules using ModuleRegistry ───────────────────
    for (ModuleAST* module : modules) {
        ctx.moduleRegistry.registerModule(moduleName, module);
    }
    ctx.moduleRegistry.setActiveModule(moduleName);

    if (ctx.options.verbose) {
        std::cout << "Loaded module: " << ctx.pool.lookup(moduleName) << "\n";
    }

    return true;
}

bool isModuleLoaded(const InterpreterContext& ctx, InternedString name) {
    return ctx.moduleRegistry.hasModule(name);
}

ModuleAST* getActiveModule(const InterpreterContext& ctx) {
    const ModuleInfo* info = ctx.moduleRegistry.getActiveModule();
    return info ? info->ast : nullptr;
}

// ─── Internal Helpers ────────────────────────────────────────────────────

InternedString generateModuleName(InterpreterContext& ctx, ModuleAST* module) {
    if (!module || !module->filePath.isValid()) {
        // Generate a unique name based on pointer
        std::string name = "module_" + std::to_string(reinterpret_cast<uintptr_t>(module));
        return ctx.pool.intern(name);
    }

    std::string name = ctx.pool.lookup(module->filePath);
    // Replace path separators and dots with underscores
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
    // codegen::generate takes: modules, StringPool&, DiagnosticEngine&, LLVMContext&
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

std::unique_ptr<llvm::Module> lowerModules(
    InterpreterContext& ctx,
    const std::vector<ModuleAST*>& modules,
    InternedString moduleName
) {
    if (modules.empty()) {
        return nullptr;
    }

    // ─── Use the CodeGen module to generate IR ──────────────────────────
    // codegen::generate takes: modules, StringPool&, DiagnosticEngine&, LLVMContext&
    auto generatedModules = codegen::generate(
        modules,
        ctx.pool,
        ctx.diagnostics,
        ctx.jit.getContext()
    );
    
    if (generatedModules.empty() || !generatedModules[0]) {
        ctx.diagnostics.error(DiagCode::Backend_CodegenError, modules[0],
                              "failed to generate IR for modules");
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Failed to generate IR for modules");
    }

    // If multiple modules were generated, we need to combine them
    // For now, we return the first one
    // TODO: Merge multiple modules into one
    return std::move(generatedModules[0]);
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

void registerModuleLibraries(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    // Delegate to the registerLibraries function from Interpreter.hpp
    registerLibraries(ctx, modules);
}

} // namespace interpreter