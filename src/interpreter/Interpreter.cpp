/**
 * @file Interpreter.cpp
 * @brief Implementation of the main interpreter engine.
 */

#include "Interpreter.hpp"

#include "../compiler/TypeMapping.hpp"
#include "../core/diagnostics/Diagnostic.hpp"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <chrono>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace interpreter {

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

Interpreter::Interpreter() = default;

Interpreter::~Interpreter() {
    // Clean up loaded modules
    m_loadedModules.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Initialization
// ─────────────────────────────────────────────────────────────────────────────

bool Interpreter::initialize(const InterpreterOptions& options) {
    if (m_initialized) {
        return true;
    }

    m_options = options;

    try {
        // 1. Initialize JIT session
        if (!m_jit.initialize()) {
            throw InterpreterError(InterpreterError::Kind::InitFailed,
                                   "Failed to initialize JIT session");
        }

        // 2. Initialize type mapping
        m_typeMapper = std::make_unique<TypeMapping>(m_jit.getContext());

        // 3. Initialize IR lowerer
        m_irLowerer = std::make_unique<IRLowering>(m_jit.getContext(), *m_typeMapper);

        // 4. Register core libraries
        // The kernel library is loaded by JITSession::setupPlatformLibraries()
        // Additional libraries will be loaded via registerForeignLibrary()

        m_initialized = true;

        if (m_options.verbose) {
            std::cout << "Interpreter initialized successfully\n";
            std::cout << "  Optimization level: " << m_options.optimizationLevel << "\n";
            std::cout << "  Debug info: " << (m_options.enableDebugInfo ? "enabled" : "disabled") << "\n";
            std::cout << "  Hot-reload: " << (m_options.enableHotReload ? "enabled" : "disabled") << "\n";
        }

        return true;

    } catch (const std::exception& e) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Initialization failed: " + std::string(e.what()));
    }
}

void Interpreter::setIRLowerer(std::unique_ptr<IRLowering> lowerer) {
    m_irLowerer = std::move(lowerer);
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Module Management
// ─────────────────────────────────────────────────────────────────────────────

ExecutionResult Interpreter::runModule(ModuleAST* module, const std::string& entryPoint) {
    if (!m_initialized) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (!module) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Cannot run null module");
    }

    // Check for frontend errors
    if (hasErrors(module)) {
        reportErrors(module);
        return ExecutionResult{1, false, "Module has semantic errors"};
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    try {
        // 1. Register foreign libraries
        registerForeignLibraries(module);

        // 2. Generate module name
        std::string moduleName = generateModuleName(module);

        // 3. Lower AST to LLVM IR
        if (m_options.verbose) {
            std::cout << "Lowering module '" << moduleName << "' to LLVM IR...\n";
        }

        auto irModule = m_irLowerer->lower(module, moduleName);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to lower module to LLVM IR");
        }

        // 4. Setup optimizations if enabled
        if (m_options.enableOptimizations) {
            setupOptimizations(irModule.get());
        }

        // 5. Add module to JIT
        if (m_options.verbose) {
            std::cout << "Adding module to JIT...\n";
        }

        if (!m_jit.addModule(std::move(irModule), moduleName)) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to add module to JIT");
        }

        // 6. Track loaded module
        m_loadedModules[moduleName] = module;
        m_activeModuleName = moduleName;
        m_hasActiveModule = true;

        // 7. Find and execute entry point
        std::string actualEntry = entryPoint.empty() ? m_options.entryPoint : entryPoint;
        std::string foundEntry = findEntryPoint(module, actualEntry);

        if (foundEntry.empty()) {
            // No entry point found - maybe it's a library module
            if (m_options.verbose) {
                std::cout << "No entry point found in module '" << moduleName << "'\n";
            }
            return ExecutionResult{0, true, ""};
        }

        if (m_options.verbose) {
            std::cout << "Executing entry point: " << foundEntry << "\n";
        }

        // 8. Execute the entry point
        int exitCode = 0;
        try {
            // Try different common signatures
            auto* fnPtr = m_jit.lookupSymbol(foundEntry);
            if (!fnPtr) {
                throw InterpreterError(InterpreterError::Kind::EntryPointNotFound,
                                       "Entry point not found: " + foundEntry);
            }

            // Try int main() first
            auto main0 = reinterpret_cast<int(*)()>(fnPtr);
            exitCode = main0();

        } catch (const std::exception& e) {
            // Runtime panic occurred
            exitCode = handlePanic(e);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        ExecutionResult result;
        result.exitCode = exitCode;
        result.success = true;
        result.executionTimeMs = duration.count() / 1000.0;

        if (m_options.verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const InterpreterError& e) {
        // Re-throw interpreter errors
        throw;
    } catch (const JITError& e) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "JIT error: " + std::string(e.what()));
    } catch (const DynLinkError& e) {
        throw InterpreterError(InterpreterError::Kind::LibraryLoadFailed,
                               "Dynamic linker error: " + std::string(e.what()));
    } catch (const std::exception& e) {
        throw InterpreterError(InterpreterError::Kind::ExecutionFailed,
                               "Execution failed: " + std::string(e.what()));
    }
}

bool Interpreter::loadModule(ModuleAST* module) {
    if (!m_initialized) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (!module) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Cannot load null module");
    }

    if (hasErrors(module)) {
        reportErrors(module);
        return false;
    }

    try {
        // 1. Register foreign libraries
        registerForeignLibraries(module);

        // 2. Generate module name
        std::string moduleName = generateModuleName(module);

        // 3. Lower AST to LLVM IR
        auto irModule = m_irLowerer->lower(module, moduleName);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to lower module to LLVM IR");
        }

        // 4. Setup optimizations if enabled
        if (m_options.enableOptimizations) {
            setupOptimizations(irModule.get());
        }

        // 5. Add module to JIT
        if (!m_jit.addModule(std::move(irModule), moduleName)) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to add module to JIT");
        }

        // 6. Track loaded module
        m_loadedModules[moduleName] = module;

        return true;

    } catch (const std::exception& e) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Module load failed: " + std::string(e.what()));
    }
}

bool Interpreter::hotReload(ModuleAST* module, const std::string& moduleName) {
    if (!m_initialized) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (!m_options.enableHotReload) {
        throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                               "Hot-reload is not enabled");
    }

    if (!module) {
        throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                               "Cannot reload null module");
    }

    if (hasErrors(module)) {
        reportErrors(module);
        return false;
    }

    try {
        // 1. Register foreign libraries (new ones)
        registerForeignLibraries(module);

        // 2. Generate versioned name
        std::string versionedName = generateVersionedName(moduleName);

        // 3. Lower AST to LLVM IR
        auto irModule = m_irLowerer->lower(module, versionedName);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                                   "Failed to lower module to LLVM IR");
        }

        // 4. Setup optimizations if enabled
        if (m_options.enableOptimizations) {
            setupOptimizations(irModule.get());
        }

        // 5. Check if old version exists
        auto it = m_moduleVersions.find(moduleName);
        if (it != m_moduleVersions.end()) {
            // Remove old version
            if (m_options.verbose) {
                std::cout << "Removing old version: " << it->second << "\n";
            }
            m_jit.removeModule(it->second);
        }

        // 6. Add new version
        if (!m_jit.addModule(std::move(irModule), versionedName)) {
            throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                                   "Failed to add module to JIT");
        }

        // 7. Update tracking
        m_moduleVersions[moduleName] = versionedName;
        m_loadedModules[versionedName] = module;

        if (m_hasActiveModule && m_activeModuleName == moduleName) {
            // Update active module
            m_activeModuleName = versionedName;
        }

        if (m_options.verbose) {
            std::cout << "Hot-reload successful: " << moduleName << " -> " << versionedName << "\n";
        }

        return true;

    } catch (const std::exception& e) {
        throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                               "Hot-reload failed: " + std::string(e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Foreign Libraries
// ─────────────────────────────────────────────────────────────────────────────

bool Interpreter::registerForeignLibrary(const std::string& libraryName) {
    try {
        if (m_dynLink.isLoaded(libraryName)) {
            return true;
        }

        if (m_options.verbose) {
            std::cout << "Loading library: " << libraryName << "\n";
        }

        m_dynLink.load(libraryName);
        m_dynLink.registerLibraryWithJIT(m_jit, libraryName);

        return true;

    } catch (const DynLinkError& e) {
        throw InterpreterError(InterpreterError::Kind::LibraryLoadFailed,
                               "Failed to load library '" + libraryName + "': " + e.what());
    }
}

void Interpreter::registerForeignLibraries(ModuleAST* module) {
    if (!module) {
        return;
    }

    // Collect all @[link] attributes from declarations
    std::vector<std::string> libraries;

    for (DeclPtr decl : module->decls) {
        for (AttributePtr attr : decl->attributes) {
            if (attr->name.toString() == "link") {
                for (AttributeArgPtr arg : attr->args) {
                    if (arg->kind == AttributeArgKind::StringLit) {
                        std::string libName = arg->value.toString();
                        // Check if it's a path or just a library name
                        bool isPath = libName.find('/') != std::string::npos ||
                                      libName.find('\\') != std::string::npos ||
                                      libName.find('.') != std::string::npos;
                        
                        if (!isPath) {
                            libraries.push_back(libName);
                        }
                    }
                }
            }
        }
    }

    // Load each library
    for (const auto& libName : libraries) {
        try {
            registerForeignLibrary(libName);
        } catch (const std::exception& e) {
            // Log but continue - the library might be loaded elsewhere
            if (m_options.verbose) {
                std::cerr << "Warning: " << e.what() << "\n";
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string Interpreter::generateModuleName(ModuleAST* module) {
    if (!module || module->filePath.isEmpty()) {
        return "module_" + std::to_string(reinterpret_cast<uintptr_t>(module));
    }
    
    std::string name = module->filePath.toString();
    // Replace path separators and extension with underscores
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    
    return name;
}

std::string Interpreter::generateVersionedName(const std::string& baseName) {
    static uint64_t versionCounter = 0;
    std::stringstream ss;
    ss << baseName << "_v" << (++versionCounter);
    return ss.str();
}

std::string Interpreter::findEntryPoint(ModuleAST* module, const std::string& entryPoint) {
    if (!module) {
        return "";
    }

    // Check if the entry point exists
    for (DeclPtr decl : module->decls) {
        if (auto* funcDecl = decl->as<FuncDeclAST>()) {
            if (funcDecl->name.toString() == entryPoint) {
                // Check if it has the @[export] attribute
                bool isExported = false;
                for (AttributePtr attr : funcDecl->attributes) {
                    if (attr->name.toString() == "export") {
                        isExported = true;
                        break;
                    }
                }
                
                // In the interpreter, we can execute non-exported functions too
                // But the entry point should be exported by convention
                if (isExported || m_options.verbose) {
                    return entryPoint;
                }
            }
        }
    }

    // Try alternative entry point names
    std::vector<std::string> alternatives = {"main", "start", "run"};
    for (const auto& alt : alternatives) {
        for (DeclPtr decl : module->decls) {
            if (auto* funcDecl = decl->as<FuncDeclAST>()) {
                if (funcDecl->name.toString() == alt) {
                    return alt;
                }
            }
        }
    }

    return "";
}

bool Interpreter::hasErrors(const ModuleAST* module) const {
    return module && module->hasErrors;
}

size_t Interpreter::reportErrors(const ModuleAST* module) const {
    if (!module) {
        return 0;
    }

    size_t count = 0;
    for (const auto& error : module->errors) {
        std::cerr << error.toString() << "\n";
        count++;
    }
    return count;
}

void Interpreter::setupOptimizations(llvm::Module* module) {
    if (!module || m_options.optimizationLevel == 0) {
        return;
    }

    // Create optimization pipeline
    llvm::PassBuilder passBuilder;
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager funcAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager modAM;

    // Register analysis managers
    passBuilder.registerModuleAnalyses(modAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(funcAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, funcAM, cgsccAM, modAM);

    // Create optimization level
    auto optLevel = static_cast<llvm::OptimizationLevel>(
        std::min(m_options.optimizationLevel, 3)
    );

    // Build optimization pipeline
    llvm::ModulePassManager modulePassManager;
    modulePassManager = passBuilder.buildPerModuleDefaultPipeline(optLevel);

    // Run optimizations
    modulePassManager.run(*module, modAM);
}

int Interpreter::handlePanic(const std::exception& exception) {
    std::cerr << "Runtime panic: " << exception.what() << "\n";
    
    // Check for specific panic types
    std::string msg = exception.what();
    if (msg.find("division by zero") != std::string::npos) {
        std::cerr << "  Division by zero occurred\n";
    } else if (msg.find("out of bounds") != std::string::npos) {
        std::cerr << "  Array index out of bounds\n";
    } else if (msg.find("null pointer") != std::string::npos) {
        std::cerr << "  Null pointer dereference\n";
    }
    
    return 1;
}

} // namespace interpreter