/**
 * @file Interpreter.cpp
 * @brief Implementation of the main interpreter engine.
 * 
 * @responsibility Provides the main orchestration for executing Lucid programs
 *                 via JIT compilation. Supports both single-module and
 *                 multi-module execution.
 */

#include "Interpreter.hpp"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <chrono>
#include <llvm/Passes/PassBuilder.h>
#include <sstream>
#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace interpreter {

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

Interpreter::~Interpreter() {
    m_loadedModulesList.clear();
    m_loadedModulesMap.clear();
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

        // 2. Initialize type mapping with string pool from JIT
        m_typeMapper = std::make_unique<TypeMapping>(m_jit.getContext(), m_jit.getStringPool());

        // 3. Initialize IR lowerer with string pool from JIT
        m_irLowerer = std::make_unique<IRLowering>(m_jit.getContext(), *m_typeMapper, m_jit.getStringPool());

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
// Interpreter - Single Module Execution
// ─────────────────────────────────────────────────────────────────────────────

ExecutionResult Interpreter::runModule(ModuleAST* module, const std::string& entryPoint) {
    if (!module) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Cannot run null module");
    }

    // Delegate to multi-module implementation
    return runModules(std::vector<ModuleAST*>{module}, entryPoint);
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Multi-Module Execution
// ─────────────────────────────────────────────────────────────────────────────

ExecutionResult Interpreter::runModules(const std::vector<ModuleAST*>& modules,
                                        const std::string& entryPoint) {
    if (!m_initialized) {
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
    }

    // Check for frontend errors
    if (hasErrors(modules)) {
        reportErrors(modules);
        return ExecutionResult{1, false, "Modules have semantic errors"};
    }

    return runImpl(modules, entryPoint);
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Internal Implementation
// ─────────────────────────────────────────────────────────────────────────────

ExecutionResult Interpreter::runImpl(const std::vector<ModuleAST*>& modules,
                                     const std::string& entryPoint) {
    auto startTime = std::chrono::high_resolution_clock::now();

    try {
        // 1. Register foreign libraries from all modules
        registerForeignLibraries(modules);

        // 2. Determine the main module name (use the first module's name)
        std::string mainModuleName = generateModuleName(modules[0]);
        std::string fullModuleName = mainModuleName;

        // 3. Lower all modules to a single LLVM module
        if (m_options.verbose) {
            std::cout << "Lowering " << modules.size() << " module(s) to LLVM IR...\n";
        }

        auto irModule = m_irLowerer->lower(modules, fullModuleName);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to lower modules to LLVM IR");
        }

        // 4. Setup optimizations if enabled
        if (m_options.enableOptimizations) {
            setupOptimizations(irModule.get());
        }

        // 5. Add module to JIT using InternedString
        if (m_options.verbose) {
            std::cout << "Adding module to JIT...\n";
        }

        // Use InternedString for module name
        InternedString modName = m_jit.getStringPool().intern(fullModuleName);
        if (!m_jit.addModule(std::move(irModule), modName)) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to add module to JIT");
        }

        // 6. Track loaded modules
        for (ModuleAST* module : modules) {
            std::string name = generateModuleName(module);
            m_loadedModulesMap[name] = module;
            m_loadedModulesList.push_back(module);
        }
        m_activeModuleName = fullModuleName;
        m_hasActiveModule = true;

        // 7. Find and execute entry point
        std::string actualEntry = entryPoint.empty() ? m_options.entryPoint : entryPoint;
        std::string foundEntry = findEntryPoint(modules, actualEntry);

        if (foundEntry.empty()) {
            if (m_options.verbose) {
                std::cout << "No entry point found in modules\n";
            }
            return ExecutionResult{0, true, "", 0.0, ""};
        }

        if (m_options.verbose) {
            std::cout << "Executing entry point: " << foundEntry << "\n";
        }

        // 8. Execute the entry point
        int exitCode = 0;
        try {
            auto* fnPtr = m_jit.lookupSymbol(foundEntry);
            if (!fnPtr) {
                throw InterpreterError(InterpreterError::Kind::EntryPointNotFound,
                                       "Entry point not found: " + foundEntry);
            }

            auto main0 = reinterpret_cast<int(*)()>(fnPtr);
            exitCode = main0();

        } catch (const std::exception& e) {
            exitCode = handlePanic(e);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);

        ExecutionResult result;
        result.exitCode = exitCode;
        result.success = true;
        result.executionTimeMs = duration.count() / 1000.0;
        result.entryPointUsed = foundEntry;

        if (m_options.verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const InterpreterError& e) {
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

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Module Loading
// ─────────────────────────────────────────────────────────────────────────────

bool Interpreter::loadModule(ModuleAST* module) {
    if (!module) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Cannot load null module");
    }

    return loadModules(std::vector<ModuleAST*>{module});
}

bool Interpreter::loadModules(const std::vector<ModuleAST*>& modules) {
    if (!m_initialized) {
        throw InterpreterError(InterpreterError::Kind::InitFailed,
                               "Interpreter not initialized");
    }

    if (modules.empty()) {
        throw InterpreterError(InterpreterError::Kind::EmptyModuleList,
                               "Cannot load empty module list");
    }

    // Validate all modules
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Cannot load null module in list");
        }
    }

    if (hasErrors(modules)) {
        reportErrors(modules);
        return false;
    }

    try {
        // 1. Register foreign libraries
        registerForeignLibraries(modules);

        // 2. Generate module name
        std::string mainModuleName = generateModuleName(modules[0]);
        std::string fullModuleName = mainModuleName;

        // 3. Lower all modules to a single LLVM module
        auto irModule = m_irLowerer->lower(modules, fullModuleName);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to lower modules to LLVM IR");
        }

        // 4. Setup optimizations if enabled
        if (m_options.enableOptimizations) {
            setupOptimizations(irModule.get());
        }

        // 5. Add module to JIT using InternedString
        InternedString modName = m_jit.getStringPool().intern(fullModuleName);
        if (!m_jit.addModule(std::move(irModule), modName)) {
            throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                                   "Failed to add module to JIT");
        }

        // 6. Track loaded modules
        for (ModuleAST* module : modules) {
            std::string name = generateModuleName(module);
            m_loadedModulesMap[name] = module;
            m_loadedModulesList.push_back(module);
        }

        return true;

    } catch (const std::exception& e) {
        throw InterpreterError(InterpreterError::Kind::ModuleLoadFailed,
                               "Module load failed: " + std::string(e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Hot-Reload
// ─────────────────────────────────────────────────────────────────────────────

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

        // 2. Generate versioned name using InternedString
        std::string versionedNameStr = generateVersionedName(moduleName);
        InternedString versionedName = m_jit.getStringPool().intern(versionedNameStr);

        // 3. Lower AST to LLVM IR
        auto irModule = m_irLowerer->lower(module, versionedNameStr);
        if (!irModule) {
            throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                                   "Failed to lower module to LLVM IR");
        }

        // 4. Setup optimizations if enabled
        if (m_options.enableOptimizations) {
            setupOptimizations(irModule.get());
        }

        // 5. Check if old version exists (using InternedString)
        InternedString oldName = m_jit.getStringPool().intern(moduleName);
        if (m_jit.hasModule(oldName)) {
            if (m_options.verbose) {
                std::cout << "Removing old version: " << moduleName << "\n";
            }
            m_jit.removeModule(oldName);
        }

        // 6. Add new version
        if (!m_jit.addModule(std::move(irModule), versionedName)) {
            throw InterpreterError(InterpreterError::Kind::HotReloadFailed,
                                   "Failed to add module to JIT");
        }

        // 7. Update tracking
        m_moduleVersions[moduleName] = versionedNameStr;
        m_loadedModulesMap[versionedNameStr] = module;

        // Update the list if this module is in it
        auto itList = std::find(m_loadedModulesList.begin(), m_loadedModulesList.end(), 
                                m_loadedModulesMap[moduleName]);
        if (itList != m_loadedModulesList.end()) {
            *itList = module;
        }

        if (m_hasActiveModule && m_activeModuleName == moduleName) {
            m_activeModuleName = versionedNameStr;
        }

        if (m_options.verbose) {
            std::cout << "Hot-reload successful: " << moduleName << " -> " << versionedNameStr << "\n";
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

void Interpreter::registerForeignLibraries(const std::vector<ModuleAST*>& modules) {
    if (modules.empty()) {
        return;
    }

    std::vector<std::string> libraries;

    for (ModuleAST* module : modules) {
        if (!module) {
            continue;
        }

        for (DeclPtr decl : module->decls) {
            for (AttributePtr attr : decl->attributes) {
                std::string_view attrName = m_jit.getStringPool().lookup(attr->name);
                if (attrName == "link") {
                    for (AttributeArgPtr arg : attr->args) {
                        if (arg->kind == AttributeArgKind::StringLit) {
                            std::string_view libName = m_jit.getStringPool().lookup(arg->value);
                            bool isPath = libName.find('/') != std::string::npos ||
                                          libName.find('\\') != std::string::npos ||
                                          libName.find('.') != std::string::npos;
                            
                            if (!isPath) {
                                libraries.push_back(std::string(libName));
                            }
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
            if (m_options.verbose) {
                std::cerr << "Warning: " << e.what() << "\n";
            }
        }
    }
}

void Interpreter::registerForeignLibraries(ModuleAST* module) {
    if (!module) {
        return;
    }
    registerForeignLibraries(std::vector<ModuleAST*>{module});
}

// ─────────────────────────────────────────────────────────────────────────────
// Interpreter - Helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string Interpreter::generateModuleName(ModuleAST* module) {
    if (!module || !module->filePath.isValid()) {
        return "module_" + std::to_string(reinterpret_cast<uintptr_t>(module));
    }
    
    std::string_view name = m_jit.getStringPool().lookup(module->filePath);
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    
    return std::string(name);
}

std::string Interpreter::generateVersionedName(const std::string& baseName) {
    static uint64_t versionCounter = 0;
    std::stringstream ss;
    ss << baseName << "_v" << (++versionCounter);
    return ss.str();
}

std::string Interpreter::findEntryPoint(const std::vector<ModuleAST*>& modules,
                                        const std::string& entryPoint) {
    if (modules.empty()) {
        return "";
    }

    // First, check the first module (usually the main module)
    if (!modules.empty()) {
        std::string found = findEntryPoint(modules[0], entryPoint);
        if (!found.empty()) {
            return found;
        }
    }

    // Then check all other modules
    for (size_t i = 1; i < modules.size(); ++i) {
        std::string found = findEntryPoint(modules[i], entryPoint);
        if (!found.empty()) {
            return found;
        }
    }

    // Try alternative entry point names
    std::vector<std::string> alternatives = {"main", "start", "run"};
    for (const auto& alt : alternatives) {
        if (alt == entryPoint) continue;
        
        for (ModuleAST* module : modules) {
            std::string found = findEntryPoint(module, alt);
            if (!found.empty()) {
                return found;
            }
        }
    }

    return "";
}

std::string Interpreter::findEntryPoint(ModuleAST* module, const std::string& entryPoint) {
    if (!module) {
        return "";
    }

    for (DeclPtr decl : module->decls) {
        if (auto* funcDecl = decl->as<FuncDeclAST>()) {
            std::string_view funcName = m_jit.getStringPool().lookup(funcDecl->name);
            if (funcName == entryPoint) {
                // Check if it has the @[export] attribute
                bool isExported = false;
                for (AttributePtr attr : funcDecl->attributes) {
                    std::string_view attrName = m_jit.getStringPool().lookup(attr->name);
                    if (attrName == "export") {
                        isExported = true;
                        break;
                    }
                }
                
                if (isExported || m_options.verbose) {
                    return entryPoint;
                }
            }
        }
    }

    return "";
}

bool Interpreter::hasErrors(const std::vector<ModuleAST*>& modules) const {
    for (const ModuleAST* module : modules) {
        if (hasErrors(module)) {
            return true;
        }
    }
    return false;
}

bool Interpreter::hasErrors(const ModuleAST* module) const {
    return module && module->hasErrors;
}

size_t Interpreter::reportErrors(const std::vector<ModuleAST*>& modules) const {
    size_t count = 0;
    for (const ModuleAST* module : modules) {
        count += reportErrors(module);
    }
    return count;
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

    llvm::PassBuilder passBuilder;
    llvm::LoopAnalysisManager loopAM;
    llvm::FunctionAnalysisManager funcAM;
    llvm::CGSCCAnalysisManager cgsccAM;
    llvm::ModuleAnalysisManager modAM;

    passBuilder.registerModuleAnalyses(modAM);
    passBuilder.registerCGSCCAnalyses(cgsccAM);
    passBuilder.registerFunctionAnalyses(funcAM);
    passBuilder.registerLoopAnalyses(loopAM);
    passBuilder.crossRegisterProxies(loopAM, funcAM, cgsccAM, modAM);

    auto optLevel = static_cast<llvm::OptimizationLevel>(
        std::min(m_options.optimizationLevel, 3)
    );

    llvm::ModulePassManager modulePassManager;
    modulePassManager = passBuilder.buildPerModuleDefaultPipeline(optLevel);
    modulePassManager.run(*module, modAM);
}

int Interpreter::handlePanic(const std::exception& exception) {
    std::cerr << "Runtime panic: " << exception.what() << "\n";
    
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