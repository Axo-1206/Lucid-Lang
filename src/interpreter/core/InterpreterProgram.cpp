/// @file core/InterpreterProgram.cpp
/// @brief Program-level state implementation.

#include "InterpreterProgram.hpp"
#include "../support/InterpreterError.hpp"
#include "codegen/CodeGen.hpp"
#include "codegen/types/LLVMTypeHelpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace interpreter {

// ─── Internal Helpers ────────────────────────────────────────────────────

static bool isFunctionExported(FuncDeclAST* func, StringPool& pool) {
    if (!func) return false;
    
    InternedString exportName = pool.intern("export");
    for (AttributeAST* attr : func->attributes) {
        if (attr->name == exportName) {
            return true;
        }
    }
    return false;
}

// ─── InterpreterProgram Implementation ───────────────────────────────────

InterpreterProgram::InterpreterProgram(InterpreterSession& session)
    : m_session(session)
    , m_registry(session.pool()) {
}

InterpreterProgram::~InterpreterProgram() {
    teardown(m_session);
}

std::unique_ptr<InterpreterProgram> InterpreterProgram::load(
    InterpreterSession& session,
    const std::vector<ModuleAST*>& modules
) {
    if (!session.isInitialized()) {
        session.initialize();
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
            session.diagnostics().error(DiagCode::Sem_ModuleNotAnalyzed, module,
                                        "module '", session.pool().lookup(module->filePath),
                                        "' has semantic errors");
            return nullptr;
        }
    }

    auto program = std::unique_ptr<InterpreterProgram>(new InterpreterProgram(session));

    // ─── 2. Assign stable module IDs ──────────────────────────────────────
    std::unordered_map<ModuleAST*, uint32_t> idMap;
    for (ModuleAST* module : modules) {
        std::string path = session.pool().lookup(module->filePath);
        uint32_t id = program->assignId(module, path);
        if (id >= session.instanceTable().size()) {
            session.diagnostics().error(DiagCode::Backend_CodegenError, module,
                                        "program exceeds maximum module capacity (",
                                        std::to_string(session.instanceTable().size()), ")");
            throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                                   "Exceeded maximum module capacity");
        }
        idMap[module] = id;
    }

    // ─── 3. Register foreign libraries ────────────────────────────────────
    session.linker().registerLibrariesFromModules(
        session.diagnostics(),
        session.pool(),
        session.options().verbose,
        modules
    );

    // ─── 4. Lower all modules via CodeGen (single pass) ───────────────────
    codegen::CodeGenOptions cgOptions;
    cgOptions.moduleCapacity = static_cast<uint32_t>(session.instanceTable().size());
    cgOptions.moduleIds = &idMap;

    std::vector<std::unique_ptr<llvm::Module>> irModules;
    try {
        irModules = codegen::generate(
            modules,
            session.pool(),
            session.diagnostics(),
            session.jit().getContext(),
            cgOptions
        );
    } catch (const std::exception& e) {
        session.diagnostics().error(DiagCode::Backend_CodegenError, nullptr,
                                    "failed to generate LLVM IR: ", e.what());
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed, e.what());
    }

    if (irModules.size() != modules.size()) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "Mismatch between AST modules and generated IR modules");
    }

    // ─── 5. Add modules to JIT and initialize instances ───────────────────
    try {
        for (size_t i = 0; i < modules.size(); ++i) {
            ModuleAST* module = modules[i];
            uint32_t id = idMap[module];
            InternedString name = program->generateModuleName(session.pool(), module);

            LoadedModule entry;
            entry.ast = module;
            entry.jitName = name;
            entry.id = id;

            // Extract imports as dependencies
            for (InternedString imp : module->imports) {
                entry.dependencies.push_back(imp);
            }

            // Add IR module to JIT and capture ResourceTracker
            entry.tracker = session.jit().addModule(std::move(irModules[i]), name);

            // Query instance size via __module_size_<sanitized>
            std::string sanitized = codegen::sanitizeForLLVMSymbol(session.pool().lookup(module->filePath));
            std::string sizeFnName = "__module_size_" + sanitized;
            void* sizeFnPtr = session.jit().lookupSymbol(sizeFnName);
            if (sizeFnPtr) {
                auto sizeFn = reinterpret_cast<uint64_t(*)()>(sizeFnPtr);
                entry.instanceSize = sizeFn();
            } else {
                entry.instanceSize = 0;
            }

            // Allocate instance buffer if needed
            if (entry.instanceSize > 0) {
                entry.instance = std::calloc(1, static_cast<size_t>(entry.instanceSize));
            } else {
                entry.instance = nullptr;
            }

            // Populate session instance table slot
            session.instanceTable()[id] = entry.instance;

            // Call __init_module_<sanitized>(instance)
            if (entry.instance) {
                std::string initFnName = "__init_module_" + sanitized;
                void* initFnPtr = session.jit().lookupSymbol(initFnName);
                if (initFnPtr) {
                    auto initFn = reinterpret_cast<void(*)(void*)>(initFnPtr);
                    initFn(entry.instance);
                }
            }

            // Register with module registry
            program->m_registry.registerModule(name, module);
            program->m_registry.setDependencies(name, entry.dependencies);

            program->m_modules.push_back(std::move(entry));
            program->m_moduleAsts.push_back(module);

            if (session.options().verbose) {
                std::cout << "Loaded module [" << id << "]: " 
                          << session.pool().lookup(module->filePath) 
                          << " (instance size: " << entry.instanceSize << " bytes)\n";
            }
        }

        if (!modules.empty()) {
            InternedString firstName = program->generateModuleName(session.pool(), modules[0]);
            program->m_registry.setActiveModule(firstName);
        }

    } catch (const std::exception& e) {
        // Rollback all already added modules and allocated instances
        program->teardown(session);
        session.diagnostics().error(DiagCode::Backend_CodegenError, nullptr,
                                    "atomic load failed, rolled back: ", e.what());
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed, e.what());
    }

    return program;
}

bool InterpreterProgram::reload(
    InterpreterSession& session,
    const std::vector<ModuleAST*>& modules
) {
    if (modules.empty()) {
        return true;
    }

    // ─── 1. Validate modules ──────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) {
            throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                                   "Cannot reload null module");
        }
        if (module->hasErrors) {
            session.diagnostics().error(DiagCode::Sem_ModuleNotAnalyzed, module,
                                        "module '", session.pool().lookup(module->filePath),
                                        "' has semantic errors");
            return false;
        }
    }

    // ─── 2. Free old instances and remove old JIT modules ─────────────────
    for (ModuleAST* module : modules) {
        std::string path = session.pool().lookup(module->filePath);
        auto it = m_idByPath.find(path);
        if (it != m_idByPath.end()) {
            uint32_t id = it->second;
            for (auto& entry : m_modules) {
                if (entry.id == id) {
                    unloadOne(session, entry);
                    break;
                }
            }
        }
    }

    // ─── 3. Register foreign libraries ────────────────────────────────────
    session.linker().registerLibrariesFromModules(
        session.diagnostics(),
        session.pool(),
        session.options().verbose,
        modules
    );

    // ─── 4. Re-assign/reuse stable IDs ────────────────────────────────────
    std::unordered_map<ModuleAST*, uint32_t> idMap;
    for (ModuleAST* module : modules) {
        std::string path = session.pool().lookup(module->filePath);
        idMap[module] = assignId(module, path);
    }

    // ─── 5. Lower new modules ─────────────────────────────────────────────
    codegen::CodeGenOptions cgOptions;
    cgOptions.moduleCapacity = static_cast<uint32_t>(session.instanceTable().size());
    cgOptions.moduleIds = &idMap;

    std::vector<std::unique_ptr<llvm::Module>> irModules = codegen::generate(
        modules,
        session.pool(),
        session.diagnostics(),
        session.jit().getContext(),
        cgOptions
    );

    if (irModules.size() != modules.size()) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Mismatch between reload modules and generated IR modules");
    }

    // ─── 6. Add new versions, allocate fresh instances, call init ─────────
    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleAST* module = modules[i];
        uint32_t id = idMap[module];
        InternedString name = generateModuleName(session.pool(), module);

        // Find existing entry or create new
        LoadedModule* entryPtr = nullptr;
        for (auto& entry : m_modules) {
            if (entry.id == id) {
                entryPtr = &entry;
                break;
            }
        }

        if (!entryPtr) {
            m_modules.emplace_back();
            entryPtr = &m_modules.back();
            m_moduleAsts.push_back(module);
        }

        entryPtr->ast = module;
        entryPtr->jitName = name;
        entryPtr->id = id;
        entryPtr->dependencies.clear();
        for (InternedString imp : module->imports) {
            entryPtr->dependencies.push_back(imp);
        }

        entryPtr->tracker = session.jit().addModule(std::move(irModules[i]), name);

        std::string sanitized = codegen::sanitizeForLLVMSymbol(session.pool().lookup(module->filePath));
        std::string sizeFnName = "__module_size_" + sanitized;
        void* sizeFnPtr = session.jit().lookupSymbol(sizeFnName);
        if (sizeFnPtr) {
            auto sizeFn = reinterpret_cast<uint64_t(*)()>(sizeFnPtr);
            entryPtr->instanceSize = sizeFn();
        } else {
            entryPtr->instanceSize = 0;
        }

        if (entryPtr->instanceSize > 0) {
            entryPtr->instance = std::calloc(1, static_cast<size_t>(entryPtr->instanceSize));
        } else {
            entryPtr->instance = nullptr;
        }

        session.instanceTable()[id] = entryPtr->instance;

        if (entryPtr->instance) {
            std::string initFnName = "__init_module_" + sanitized;
            void* initFnPtr = session.jit().lookupSymbol(initFnName);
            if (initFnPtr) {
                auto initFn = reinterpret_cast<void(*)(void*)>(initFnPtr);
                initFn(entryPtr->instance);
            }
        }

        m_registry.registerModule(name, module);
        m_registry.setDependencies(name, entryPtr->dependencies);

        if (session.options().verbose) {
            std::cout << "Hot-reloaded module [" << id << "]: " 
                      << session.pool().lookup(module->filePath) << "\n";
        }
    }

    return true;
}

ExecutionResult InterpreterProgram::run(
    InterpreterSession& session,
    InternedString entryPoint
) {
    if (!session.jit().isInitialized()) {
        throw InterpreterError(InterpreterErrorKind::InitFailed,
                               "Interpreter not initialized");
    }

    if (m_modules.empty()) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "No modules loaded");
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    try {
        // If no entry point specified, default to "main"
        if (!entryPoint.isValid()) {
            entryPoint = session.pool().intern("main");
        }

        InternedString foundEntry = findEntryPoint(session, entryPoint);
        if (!foundEntry.isValid()) {
            session.diagnostics().error(DiagCode::Sem_UndefinedValue, nullptr,
                                        "entry point '", session.pool().lookup(entryPoint), 
                                        "' not found in any loaded module");
            throw InterpreterError(InterpreterErrorKind::EntryPointNotFound,
                                   "Entry point '" + session.pool().lookup(entryPoint) + 
                                   "' not found in any loaded module");
        }

        std::string foundName = session.pool().lookup(foundEntry);
        void* fnPtr = session.jit().lookupSymbol(foundName);
        if (!fnPtr) {
            session.diagnostics().error(DiagCode::Ffi_UnknownSymbol, nullptr,
                                        "symbol '", foundName, "' not found in JIT");
            throw InterpreterError(InterpreterErrorKind::SymbolLookupFailed,
                                   "Symbol lookup failed: " + foundName);
        }

        int exitCode = 0;
        try {
            auto mainFn = reinterpret_cast<int(*)()>(fnPtr);
            exitCode = mainFn();
        } catch (const std::exception& e) {
            exitCode = session.panicHandler().handle(e);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime);

        ExecutionResult result;
        result.exitCode = exitCode;
        result.success = true;
        result.executionTimeMs = duration.count() / 1000.0;
        result.entryPointUsed = foundName;

        if (session.options().verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const InterpreterError& e) {
        throw;
    } catch (const std::exception& e) {
        session.diagnostics().error(DiagCode::Backend_CodegenError, nullptr,
                                    "execution failed: ", e.what());
        throw InterpreterError(InterpreterErrorKind::ExecutionFailed, e.what());
    }
}

void InterpreterProgram::teardown(InterpreterSession& session) {
    // Unload all modules in reverse order
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
        unloadOne(session, *it);
    }
    m_modules.clear();
    m_moduleAsts.clear();
    m_registry.clear();
}

void InterpreterProgram::unloadOne(InterpreterSession& session, LoadedModule& entry) {
    if (entry.instance) {
        if (entry.ast) {
            std::string sanitized = codegen::sanitizeForLLVMSymbol(session.pool().lookup(entry.ast->filePath));
            std::string freeFnName = "__free_module_" + sanitized;
            void* freeFnPtr = session.jit().lookupSymbol(freeFnName);
            if (freeFnPtr) {
                auto freeFn = reinterpret_cast<void(*)(void*)>(freeFnPtr);
                freeFn(entry.instance);
            }
        }
        std::free(entry.instance);
        entry.instance = nullptr;
    }

    if (entry.id < session.instanceTable().size()) {
        session.instanceTable()[entry.id] = nullptr;
    }

    if (entry.tracker) {
        session.jit().removeModule(entry.tracker);
        entry.tracker = nullptr;
    }
}

uint32_t InterpreterProgram::assignId(ModuleAST* module, const std::string& path) {
    auto it = m_idByPath.find(path);
    if (it != m_idByPath.end()) {
        m_idByAst[module] = it->second;
        return it->second;
    }

    uint32_t id = m_nextId++;
    m_idByPath[path] = id;
    m_idByAst[module] = id;
    return id;
}

InternedString InterpreterProgram::generateModuleName(StringPool& pool, ModuleAST* module) {
    if (!module || !module->filePath.isValid()) {
        std::string name = "module_" + 
                          std::to_string(reinterpret_cast<uintptr_t>(module));
        return pool.intern(name);
    }

    std::string name = pool.lookup(module->filePath);
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '\\', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    
    return pool.intern(name);
}

InternedString InterpreterProgram::findEntryPoint(InterpreterSession& session, InternedString entryPoint) {
    if (entryPoint.isValid()) {
        for (ModuleInfo* info : m_registry.getAllModules()) {
            ModuleAST* module = info ? info->ast : nullptr;
            if (!module) continue;

            for (DeclAST* decl : module->decls) {
                if (FuncDeclAST* func = decl->as<FuncDeclAST>()) {
                    if (func->name == entryPoint) {
                        if (isFunctionExported(func, session.pool()) || func->name == session.pool().intern("main")) {
                            return entryPoint;
                        }
                    }
                }
            }
        }
    }
    return InternedString();
}

ModuleAST* InterpreterProgram::moduleForId(uint32_t id) const {
    for (const auto& entry : m_modules) {
        if (entry.id == id) {
            return entry.ast;
        }
    }
    return nullptr;
}

ModuleAST* InterpreterProgram::findByPath(const std::string& path) const {
    auto it = m_idByPath.find(path);
    if (it != m_idByPath.end()) {
        return moduleForId(it->second);
    }
    return nullptr;
}

const InterpreterProgram::LoadedModule* InterpreterProgram::getLoadedModule(uint32_t id) const {
    for (const auto& entry : m_modules) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace interpreter
