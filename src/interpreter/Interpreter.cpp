/// @file Interpreter.cpp
/// @brief Implementation of the Interpreter facade and compatibility API.

#include "Interpreter.hpp"
#include "support/InterpreterError.hpp"

#include <iostream>

namespace interpreter {

// =============================================================================
// Interpreter Facade Class
// =============================================================================

Interpreter::Interpreter(StringPool& pool, DiagnosticEngine& diag,
                         const InterpreterOptions& options)
    : m_session(std::make_unique<InterpreterSession>(pool, diag, options)) {
}

Interpreter::~Interpreter() = default;

void Interpreter::initialize() {
    m_session->initialize();
}

bool Interpreter::isInitialized() const {
    return m_session->isInitialized();
}

bool Interpreter::load(const std::vector<ModuleAST*>& modules) {
    if (!m_session->isInitialized()) {
        m_session->initialize();
    }
    m_program = InterpreterProgram::load(*m_session, modules);
    return m_program != nullptr;
}

bool Interpreter::reload(const std::vector<ModuleAST*>& modules) {
    if (!m_program) {
        return load(modules);
    }
    return m_program->reload(*m_session, modules);
}

bool Interpreter::hotReload(ModuleAST* module, InternedString name) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Cannot reload null module");
    }

    if (!m_program) {
        return load({module});
    }

    if (!m_session->options().enableHotReload) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Hot-reload is not enabled");
    }

    std::vector<ModuleInfo*> affected = m_program->registry().getAffectedModules(name);
    std::vector<ModuleAST*> toReload;
    toReload.push_back(module);
    for (ModuleInfo* info : affected) {
        if (info && info->ast && info->ast != module) {
            toReload.push_back(info->ast);
        }
    }

    if (m_session->options().verbose) {
        std::cout << "Hot-reloading " << toReload.size() << " module(s)\n";
    }

    return m_program->reload(*m_session, toReload);
}

ExecutionResult Interpreter::run(InternedString entryPoint) {
    if (!m_program) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "No program loaded to run");
    }
    return m_program->run(*m_session, entryPoint);
}

// =============================================================================
// Procedural Convenience API
// =============================================================================

void initialize(InterpreterContext& ctx, const InterpreterOptions& options) {
    ctx.options = options;
    ctx.session.options() = options;
    ctx.session.initialize();
}

bool isInitialized(const InterpreterContext& ctx) {
    return ctx.session.isInitialized();
}

ExecutionResult runModules(InterpreterContext& ctx,
                           const std::vector<ModuleAST*>& modules,
                           InternedString entryPoint,
                           bool isHotReload) {
    if (!ctx.session.isInitialized()) {
        ctx.session.initialize();
    }

    if (isHotReload && ctx.program) {
        if (!ctx.program->reload(ctx.session, modules)) {
            return ExecutionResult{1, false, "Failed to reload modules"};
        }
    } else {
        ctx.program = InterpreterProgram::load(ctx.session, modules);
        if (!ctx.program) {
            return ExecutionResult{1, false, "Failed to load modules"};
        }
    }

    return ctx.program->run(ctx.session, entryPoint);
}

ExecutionResult runModule(InterpreterContext& ctx, ModuleAST* module, 
                          InternedString entryPoint,
                          bool isHotReload) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::EmptyModuleList,
                               "Cannot run null module");
    }
    return runModules(ctx, std::vector<ModuleAST*>{module}, entryPoint, isHotReload);
}

bool loadModules(InterpreterContext& ctx, const std::vector<ModuleAST*>& modules) {
    if (!ctx.session.isInitialized()) {
        ctx.session.initialize();
    }
    ctx.program = InterpreterProgram::load(ctx.session, modules);
    return ctx.program != nullptr;
}

bool loadModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        return false;
    }
    return loadModules(ctx, {module});
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, InternedString name) {
    if (!module) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Cannot reload null module");
    }

    if (!ctx.program) {
        return loadModules(ctx, {module});
    }

    if (!ctx.options.enableHotReload && !ctx.session.options().enableHotReload) {
        throw InterpreterError(InterpreterErrorKind::HotReloadFailed,
                               "Hot-reload is not enabled");
    }

    std::vector<ModuleInfo*> affected = ctx.program->registry().getAffectedModules(name);
    std::vector<ModuleAST*> toReload;
    toReload.push_back(module);
    for (ModuleInfo* info : affected) {
        if (info && info->ast && info->ast != module) {
            toReload.push_back(info->ast);
        }
    }

    return ctx.program->reload(ctx.session, toReload);
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name) {
    InternedString nameInterned = ctx.pool.intern(name);
    return hotReloadModule(ctx, module, nameInterned);
}

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx) {
    return ctx.program ? ctx.program->registry().getAllModules() : std::vector<ModuleInfo*>{};
}

} // namespace interpreter