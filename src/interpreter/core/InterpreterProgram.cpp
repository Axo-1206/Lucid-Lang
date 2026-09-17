/// @file core/InterpreterProgram.cpp
/// @brief Program-level state implementation.

#include "InterpreterProgram.hpp"
#include "../support/InterpreterError.hpp"
#include "codegen/CodeGen.hpp"
#include "codegen/types/LLVMTypeHelpers.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace interpreter {

// ─── InterpreterProgram Implementation ───────────────────────────────────

InterpreterProgram::InterpreterProgram(InterpreterSession& session)
    : m_registry(session.pool()) {
    // The session is used only to obtain the pool for the registry.
    // It is deliberately not stored — see the class doc comment in the
    // header for the lifetime contract.
}

InterpreterProgram::~InterpreterProgram() {
    // teardown(session) must have been called by the owner (Interpreter
    // or InterpreterContext) before this destructor runs, while the
    // session was still alive. The destructor has no session parameter
    // and therefore cannot tear down; if m_modules is non-empty here,
    // the owner violated the contract and resources leaked.
    assert(m_modules.empty() &&
           "InterpreterProgram destroyed without teardown()");
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
            program->m_registry.setDependencies(name, module->imports);

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

    // ══════════════════════════════════════════════════════════════════════
    // Reload is structured as validate → stage → commit.
    //
    // ─── Why This Structure ──────────────────────────────────────────────
    //
    // ORC's main JITDylib is a flat symbol namespace: two modules cannot
    // both define the same symbol. So "load the new version alongside the
    // old, then swap" is not possible — removing the old module is a
    // precondition for installing the new one. That makes reload
    // inherently destructive at the JIT level.
    //
    // To keep the observable failure mode as close to atomic as the JIT
    // allows, every failure-prone step runs BEFORE the first destructive
    // action:
    //
    //   Stage   — validate inputs, assign IDs, run codegen (this is where
    //             the overwhelming majority of failures live: sema errors
    //             on the new AST, IR generation errors, IR verification
    //             errors), and query each module's instance size. None of
    //             this mutates the JIT, the instance table, or the
    //             registry. If stage fails, the old program is fully
    //             intact and this function returns false with no cleanup
    //             needed.
    //
    //   Commit  — the destructive loop: unload old, add new, allocate
    //             instance, call init, update registry. Each step here is
    //             fast and, given that stage succeeded, unlikely to fail.
    //             If a commit step does fail partway through, the program
    //             is left in a mixed state (modules 0..k-1 reloaded,
    //             modules k..n still on their old versions). This window
    //             is documented and accepted; recovering from a commit-
    //             phase failure requires the caller to reload from
    //             scratch, which is the only thing that can be done
    //             without a multi-JITDylib redesign of JITSession.
    // ══════════════════════════════════════════════════════════════════════

    // ─── Phase 0: Validate ────────────────────────────────────────────────
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

    // ─── Phase 1: Stage ───────────────────────────────────────────────────

    // 1a. Reuse/create stable IDs.
    //
    //     Keyed on AST pointer, not file path (#4 in the audit). The path
    //     can legitimately change across reloads (file moved, module
    //     renamed, caller constructed a fresh AST for the same module
    //     location); the AST pointer is the caller's identity for the
    //     module being reloaded. m_idByAst was already maintained by
    //     assignId; this change makes reload actually use it.
    //
    //     Also enforces the same bound check that load() does (#9) so a
    //     newly-discovered module path can't silently index past the
    //     instance table.
    std::unordered_map<ModuleAST*, uint32_t> idMap;
    for (ModuleAST* module : modules) {
        std::string path = session.pool().lookup(module->filePath);
        uint32_t id = assignId(module, path);
        if (id >= session.instanceTable().size()) {
            session.diagnostics().error(DiagCode::Backend_CodegenError, module,
                                        "program exceeds maximum module capacity (",
                                        std::to_string(session.instanceTable().size()), ")");
            throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                                   "Exceeded maximum module capacity");
        }
        idMap[module] = id;
    }

    // 1b. Register foreign libraries. This can fail (missing library,
    //     bad symbol table) and must happen before any destructive step
    //     so a failure leaves the old modules untouched.
    session.linker().registerLibrariesFromModules(
        session.diagnostics(),
        session.pool(),
        session.options().verbose,
        modules
    );

    // 1c. Lower new modules to IR. The expensive, failure-prone step.
    //     Produces IR but does not touch the JIT.
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

    // ══════════════════════════════════════════════════════════════════════
    // Phase 2: Commit — destructive.
    //
    // Everything below mutates the JIT, the instance table, or the
    // registry. Failures here leave a mixed state.
    // ══════════════════════════════════════════════════════════════════════

    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleAST* module = modules[i];
        uint32_t id = idMap[module];
        InternedString name = generateModuleName(session.pool(), module);

        // 2a. Find the existing entry (must exist: stage assigned its ID
        //     by reusing or creating the idByAst/idByPath mapping, and
        //     either path leaves the entry in m_modules for existing
        //     modules, or leaves it absent for genuinely-new ones).
        LoadedModule* entryPtr = nullptr;
        for (auto& entry : m_modules) {
            if (entry.id == id) {
                entryPtr = &entry;
                break;
            }
        }

        // 2b. Unload the old version FIRST, so its symbols leave the JIT
        //     before the new version's symbols are added. Doing this before
        //     touching the instance or the slot keeps unloadOne itself
        //     atomic (#5 in the audit): if removeModule throws, the entry
        //     is untouched and a retry is possible.
        if (entryPtr) {
            unloadOne(session, *entryPtr);
        } else {
            // New module, not seen before: create the entry now.
            m_modules.emplace_back();
            entryPtr = &m_modules.back();
            m_moduleAsts.push_back(module);
        }

        entryPtr->ast = module;
        entryPtr->jitName = name;
        entryPtr->id = id;
        entryPtr->tracker = nullptr;
        entryPtr->instance = nullptr;
        entryPtr->instanceSize = 0;

        // 2c. Add new IR to the JIT.
        entryPtr->tracker = session.jit().addModule(std::move(irModules[i]), name);

        // 2d. Query the new module's instance size and allocate.
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

        // 2e. Initialize the instance.
        if (entryPtr->instance) {
            std::string initFnName = "__init_module_" + sanitized;
            void* initFnPtr = session.jit().lookupSymbol(initFnName);
            if (initFnPtr) {
                auto initFn = reinterpret_cast<void(*)(void*)>(initFnPtr);
                initFn(entryPtr->instance);
            }
        }

        // 2f. Update the registry.
        m_registry.registerModule(name, module);
        m_registry.setDependencies(name, module->imports);

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

        // ─── Resolve the entry point to a FuncDeclAST, then to a symbol ──
        //
        // findEntryPoint returns the declaration (not a name string)
        // because the symbol to look up is decl->mangledName, not
        // decl->name. For an @[export]ed function, Sema's mangling makes
        // mangledName == name, so the two coincide; for any other
        // function, they do not, and looking up decl->name would fail
        // (see MangledName.cpp for the export-aware mangling rules).
        //
        // The previous implementation looked up the source name and had
        // a fallback that treated a bare "main" without @[export] as a
        // valid entry point. That fallback was wrong: a non-exported
        // "main" is mangled to `_L..._main_P..._R...` and is not the
        // program's entry point at all. Only @[export] const main is.
        FuncDeclAST* entryDecl = findEntryPoint(session, entryPoint);
        if (!entryDecl) {
            session.diagnostics().error(DiagCode::Sem_UndefinedValue, nullptr,
                                        "entry point '", session.pool().lookup(entryPoint),
                                        "' not found in any loaded module");
            throw InterpreterError(InterpreterErrorKind::EntryPointNotFound,
                                   "Entry point '" + session.pool().lookup(entryPoint) +
                                   "' not found in any loaded module");
        }

        std::string foundName = session.pool().lookup(entryDecl->mangledName);
        void* fnPtr = session.jit().lookupSymbol(foundName);
        if (!fnPtr) {
            session.diagnostics().error(DiagCode::Ffi_UnknownSymbol, nullptr,
                                        "symbol '", foundName, "' not found in JIT");
            throw InterpreterError(InterpreterErrorKind::SymbolLookupFailed,
                                   "Symbol lookup failed: " + foundName);
        }

        // ─── Invoke the entry point ─────────────────────────────────────
        //
        // No try/catch around the call. Under Design A, panics terminate
        // the process (__lucid_panic calls std::abort) and foreign
        // functions are C-only, so nothing reachable from mainFn()
        // throws a C++ exception. If something does throw, it's a bug
        // in the interpreter or in a runtime helper, and letting it
        // propagate to the outer catch — where it becomes a diagnostic
        // and an ExecutionFailed — is the correct response. Swallowing
        // it into exitCode = 1 would hide the bug.
        auto mainFn = reinterpret_cast<int(*)()>(fnPtr);
        int exitCode = mainFn();

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            endTime - startTime);

        ExecutionResult result;
        result.exitCode = exitCode;
        result.success = (exitCode == 0);
        result.executionTimeMs = duration.count() / 1000.0;
        result.entryPointUsed = foundName;

        if (session.options().verbose) {
            std::cout << "Execution completed in " << result.executionTimeMs << "ms\n";
            std::cout << "Exit code: " << exitCode << "\n";
        }

        return result;

    } catch (const std::exception& e) {
        // Catches real failures — codegen errors thrown by lower layers,
        // symbol lookup failures, and (via the InterpreterError below)
        // the specific interpreter errors raised above. The redundant
        // `catch (const InterpreterError&) { throw; }` from the previous
        // version has been removed; InterpreterError derives from
        // std::runtime_error and would be caught here anyway, so the
        // explicit re-throw was a no-op that obscured the control flow.
        //
        // If the exception is already an InterpreterError, propagate it
        // unchanged so callers see the original kind. Otherwise wrap it.
        if (auto* ie = dynamic_cast<const InterpreterError*>(&e)) {
            throw *ie;
        }
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
    // ─── Order: free-instance → null-slot → remove-module ────────────────
    //
    // __free_module_<sanitized> lives inside the module being removed, so
    // it must be called before removeModule. The order below reflects
    // that dependency and is the only correct order given ORC's symbol
    // ownership model.
    //
    // removeModule is called last and does NOT throw — see
    // JITSession::removeModule. If it fails, it logs and returns false,
    // and this function has already completed its own bookkeeping
    // (instance freed, slot nulled), so the entry is left clean. The
    // stale tracker in the JITDylib is reclaimed when the session is
    // destroyed; nothing can reach it in the meantime because the
    // instance-table slot that would have pointed at its data is null.
    if (entry.instance) {
        if (entry.ast) {
            std::string sanitized = codegen::sanitizeForLLVMSymbol(
                session.pool().lookup(entry.ast->filePath));
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

FuncDeclAST* InterpreterProgram::findEntryPoint(InterpreterSession& session, InternedString entryPoint) {
    if (!entryPoint.isValid()) {
        return nullptr;
    }

    // ─── Entry point contract ────────────────────────────────────────────
    //
    // An entry point is by definition an @[export]ed function. Sema's
    // attribute validator enforces that @[export] is module-level-only,
    // and Sema's mangling (MangledName.cpp) makes an exported function's
    // mangledName equal to its source name. So the returned FuncDeclAST's
    // mangledName is the exact symbol the JIT will have.
    //
    // The previous implementation also accepted a bare "main" without
    // @[export] as an entry point. That was wrong: a non-exported main is
    // mangled to `_L..._main_P..._R...`, and looking up "main" in the JIT
    // would fail. Dropping the fallback makes the contract explicit:
    // entry points must be @[export]ed, full stop.
    for (ModuleInfo* info : m_registry.getAllModules()) {
        ModuleAST* module = info ? info->ast : nullptr;
        if (!module) continue;

        for (DeclAST* decl : module->decls) {
            if (!decl->isa<FuncDeclAST>()) continue;
            FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->name == entryPoint && func->isExported) {
                return func;
            }
        }
    }
    return nullptr;
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
