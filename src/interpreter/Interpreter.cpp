/// @file Interpreter.cpp
/// @brief Implementation of the main interpreter API.

#include "Interpreter.hpp"
#include "support/InterpreterError.hpp"
#include "execution/ModuleLoader.hpp"
#include "execution/SymbolResolver.hpp"
#include "core/ModuleRegistry.hpp"

#include "codegen/CodeGen.hpp"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Verifier.h"

#include <chrono>
#include <iostream>

namespace interpreter {

// ─── Construction ──────────────────────────────────────────────────────────

Interpreter::Interpreter(StringPool& pool, DiagnosticEngine& diagnostics)
    : m_context(std::make_unique<InterpreterContext>(pool, diagnostics)) {}

// ─── Initialization ────────────────────────────────────────────────────────

void Interpreter::initialize(const InterpreterOptions& options) {
    if (m_context->initialized) {
        return;
    }

    m_context->options = options;

    try {
        m_context->jit.initialize();
        m_context->initialized = true;

        if (options.verbose) {
            std::cout << "Interpreter initialized successfully\n";
            std::cout << "  Optimization level: " << options.optimizationLevel << "\n";
            std::cout << "  Debug info: " << (options.enableDebugInfo ? "enabled" : "disabled") << "\n";
            std::cout << "  Hot-reload: " << (options.enableHotReload ? "enabled" : "disabled") << "\n";
        }
    } catch (const std::exception& e) {
        m_context->diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                                     "interpreter initialization failed: ", e.what());
        throw InterpreterError(InterpreterError::Kind::InitFailed, e.what());
    }
}

// ─── Run ──────────────────────────────────────────────────────────────────

ExecutionResult Interpreter::run(ModuleAST* module, InternedString entryPoint) {
    if (!module) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot run null module");
    }
    return run(std::vector<ModuleAST*>{module}, entryPoint);
}

ExecutionResult Interpreter::run(ModuleAST* module, const std::string& entryPoint) {
    InternedString ep = entryPoint.empty() 
        ? InternedString() 
        : m_context->pool.intern(entryPoint);
    return run(module, ep);
}

ExecutionResult Interpreter::run(const std::vector<ModuleAST*>& modules, 
                                 InternedString entryPoint) {
    if (!m_context->initialized) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot run empty module list");
    }

    // Validate all modules
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Cannot run null module in list");
        }
        if (module->hasErrors) {
            // Report errors using the diagnostic system
            m_context->diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                                         "module '", 
                                         m_context->pool.lookup(module->filePath),
                                         "' has semantic errors");
            return ExecutionResult{1, false, "Module has semantic errors"};
        }
    }

    return runImpl(modules, entryPoint);
}

ExecutionResult Interpreter::run(const std::vector<ModuleAST*>& modules,
                                 const std::string& entryPoint) {
    InternedString ep = entryPoint.empty() 
        ? InternedString() 
        : m_context->pool.intern(entryPoint);
    return run(modules, ep);
}

// ─── Load ──────────────────────────────────────────────────────────────────

bool Interpreter::load(ModuleAST* module) {
    if (!module) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Cannot load null module");
    }
    return load(std::vector<ModuleAST*>{module});
}

bool Interpreter::load(const std::vector<ModuleAST*>& modules) {
    if (!m_context->initialized) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot load empty module list");
    }

    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Cannot load null module in list");
        }
        if (module->hasErrors) {
            m_context->diagnostics.error(DiagCode::Sem_ModuleNotAnalyzed, module,
                                         "module '", 
                                         m_context->pool.lookup(module->filePath),
                                         "' has semantic errors");
            return false;
        }
    }

    try {
        ModuleLoader loader(*m_context);
        return loader.loadModules(modules);
    } catch (const std::exception& e) {
        m_context->diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                                     "module load failed: ", e.what());
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed, e.what());
    }
}

// ─── Internal Implementation ──────────────────────────────────────────────

ExecutionResult Interpreter::runImpl(const std::vector<ModuleAST*>& modules,
                                     InternedString entryPoint) {
    auto startTime = std::chrono::high_resolution_clock::now();

    try {
        // 1. Register foreign libraries
        registerLibraries(modules);

        // 2. Load modules
        ModuleLoader loader(*m_context);
        if (!loader.loadModules(modules)) {
            return ExecutionResult{1, false, "Failed to load modules"};
        }

        // 3. Find entry point
        SymbolResolver resolver(*m_context);
        InternedString foundEntry = resolver.findEntryPoint(entryPoint);
        if (!foundEntry.isValid()) {
            std::string epName = entryPoint.isValid() 
                ? m_context->pool.lookup(entryPoint)
                : m_context->options.entryPoint;
            reportEntryPointNotFound(m_context->diagnostics, epName);
            return ExecutionResult{1, false, "Entry point not found: " + epName};
        }

        // 4. Execute entry point
        void* fnPtr = resolver.lookupSymbol(foundEntry);
        if (!fnPtr) {
            reportSymbolLookupError(m_context->diagnostics,
                                   m_context->pool.lookup(foundEntry),
                                   "symbol not found in JIT");
            return ExecutionResult{1, false, "Symbol lookup failed"};
        }

        int exitCode = 0;
        try {
            auto mainFn = reinterpret_cast<int(*)()>(fnPtr);
            exitCode = mainFn();
        } catch (const std::exception& e) {
            exitCode = m_context->panicHandler.handle(e);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime);

        ExecutionResult result;
        result.exitCode = exitCode;
        result.success = true;
        result.executionTimeMs = duration.count() / 1000.0;
        result.entryPointUsed = m_context->pool.lookup(foundEntry);

        if (m_context->options.verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const InterpreterError& e) {
        if (e.hasCode()) {
            e.report(m_context->diagnostics);
        }
        throw;
    } catch (const std::exception& e) {
        m_context->diagnostics.error(DiagCode::Backend_CodegenError, nullptr,
                                     "execution failed: ", e.what());
        throw InterpreterError(InterpreterError::Kind::ExecutionFailed, e.what());
    }
}

InternedString Interpreter::findEntryPoint(const std::vector<ModuleAST*>& modules,
                                           InternedString entryPoint) {
    SymbolResolver resolver(*m_context);
    return resolver.findEntryPoint(entryPoint);
}

// ─── Foreign Libraries ────────────────────────────────────────────────────

void Interpreter::registerLibrary(const std::string& name) {
    try {
        if (m_context->linker.isLoaded(name)) {
            return;
        }
        m_context->linker.load(name);
        m_context->linker.registerWithJIT(m_context->jit);
    } catch (const std::exception& e) {
        reportLibraryLoadError(m_context->diagnostics, name, e.what());
        throw InterpreterError(InterpreterError::Kind::LibraryLoadFailed, e.what());
    }
}

void Interpreter::registerLibraries(ModuleAST* module) {
    if (!module) return;
    registerLibraries(std::vector<ModuleAST*>{module});
}

void Interpreter::registerLibraries(const std::vector<ModuleAST*>& modules) {
    for (ModuleAST* module : modules) {
        registerLibrariesFromModule(module);
    }
}

void Interpreter::registerLibrariesFromModule(ModuleAST* module) {
    if (!module) return;

    for (DeclAST* decl : module->decls) {
        for (AttributePtr attr : decl->attributes) {
            if (attr->name == m_context->pool.intern("link")) {
                for (LiteralExprAST* arg : attr->args) {
                    if (arg->kind == LiteralKind::String || 
                        arg->kind == LiteralKind::RawString) {
                        std::string libName = m_context->pool.lookup(arg->value);
                        // Skip if it looks like a file path (has extension)
                        bool isPath = libName.find('/') != std::string::npos ||
                                      libName.find('\\') != std::string::npos ||
                                      libName.find('.') != std::string::npos;
                        if (!isPath) {
                            try {
                                registerLibrary(libName);
                            } catch (const std::exception& e) {
                                if (m_context->options.verbose) {
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

// ─── Symbol Lookup ─────────────────────────────────────────────────────────

void* Interpreter::lookupSymbol(const std::string& name) {
    if (!m_context->initialized) {
        return nullptr;
    }
    try {
        return m_context->jit.lookupSymbol(name);
    } catch (const std::exception& e) {
        reportSymbolLookupError(m_context->diagnostics, name, e.what());
        return nullptr;
    }
}

void* Interpreter::lookupSymbol(InternedString name) {
    return lookupSymbol(m_context->pool.lookup(name));
}

// ─── Accessors ─────────────────────────────────────────────────────────────

std::vector<ModuleInfo*> Interpreter::getLoadedModules() {
    return m_context->modules.getAllModules();
}

} // namespace interpreter