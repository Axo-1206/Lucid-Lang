/// @file Interpreter.cpp
/// @brief Implementation of the Interpreter facade and compatibility API.

#include "Interpreter.hpp"
#include "support/InterpreterError.hpp"

#include <iostream>

namespace interpreter {

// =============================================================================
// Internal Helpers
// =============================================================================

namespace {

/// Load a program into `program`, tearing down any existing one first.
/// Returns true on success. On failure, `program` is left reset (null).
///
/// Shared by Interpreter::load and the procedural loadModules(). Both
/// call sites pass an owned unique_ptr they want replaced, so the
/// teardown-if-present logic lives here once.
bool loadInto(InterpreterSession& session,
              std::unique_ptr<InterpreterProgram>& program,
              const std::vector<ModuleAST*>& modules) {
    // Tear down any existing program while the session is alive.
    if (program) {
        program->teardown(session);
        program.reset();
    }

    program = InterpreterProgram::load(session, modules);
    return program != nullptr;
}

/// Run the entry point of an already-loaded program.
/// Throws if the program is null (caller should check).
ExecutionResult runWith(InterpreterSession& session,
                        InterpreterProgram* program,
                        InternedString entryPoint) {
    if (!program) {
        throw InterpreterError(InterpreterErrorKind::ModuleLoadFailed,
                               "No program loaded to run");
    }
    return program->run(session, entryPoint);
}

/// Hot-reload a module and its dependents into `program`, creating a new
/// program from just this module if none is loaded. Returns false on any
/// precondition failure (null module, hot-reload disabled).
///
/// Note: the "hot-reload disabled" check reads the session's options.
/// InterpreterContext::options duplicates the session's options and the
/// two can diverge; the session's are the ones initialize() actually
/// installs, so they are the source of truth.
bool hotReloadInto(InterpreterSession& session,
                   std::unique_ptr<InterpreterProgram>& program,
                   ModuleAST* module,
                   InternedString name) {
    if (!module) {
        return false;
    }

    // No program loaded yet — treat as an initial load.
    if (!program) {
        std::vector<ModuleAST*> single{module};
        return loadInto(session, program, single);
    }

    if (!session.options().enableHotReload) {
        return false;
    }

    // Collect the changed module plus all dependents.
    std::vector<ModuleInfo*> affected = program->registry().getAffectedModules(name);
    std::vector<ModuleAST*> toReload;
    toReload.push_back(module);
    for (ModuleInfo* info : affected) {
        if (info && info->ast && info->ast != module) {
            toReload.push_back(info->ast);
        }
    }

    if (session.options().verbose) {
        std::cout << "Hot-reloading " << toReload.size() << " module(s)\n";
    }

    return program->reload(session, toReload);
}

} // anonymous namespace

// =============================================================================
// Interpreter Facade Class
// =============================================================================

Interpreter::Interpreter(StringPool& pool, DiagnosticEngine& diag,
                         const InterpreterOptions& options)
    : m_session(std::make_unique<InterpreterSession>(pool, diag, options)) {
}

Interpreter::~Interpreter() {
    // Tear down the program while the session is still alive. Member
    // destruction order (session declared first, destroyed last) means
    // both are valid in this destructor body. m_program.reset() runs
    // ~InterpreterProgram, which asserts that teardown was called.
    closeProgram();
}

void Interpreter::closeProgram() {
    if (m_program) {
        m_program->teardown(*m_session);
        m_program.reset();
    }
}

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
    return loadInto(*m_session, m_program, modules);
}

bool Interpreter::reload(const std::vector<ModuleAST*>& modules) {
    if (!m_program) {
        return load(modules);
    }
    return m_program->reload(*m_session, modules);
}

bool Interpreter::hotReload(ModuleAST* module, InternedString name) {
    return hotReloadInto(*m_session, m_program, module, name);
}

ExecutionResult Interpreter::run(InternedString entryPoint) {
    return runWith(*m_session, m_program.get(), entryPoint);
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
        if (!loadInto(ctx.session, ctx.program, modules)) {
            return ExecutionResult{1, false, "Failed to load modules"};
        }
    }

    return runWith(ctx.session, ctx.program.get(), entryPoint);
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
    return loadInto(ctx.session, ctx.program, modules);
}

bool loadModule(InterpreterContext& ctx, ModuleAST* module) {
    if (!module) {
        return false;
    }
    return loadModules(ctx, {module});
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, InternedString name) {
    return hotReloadInto(ctx.session, ctx.program, module, name);
}

bool hotReloadModule(InterpreterContext& ctx, ModuleAST* module, const std::string& name) {
    InternedString nameInterned = ctx.pool.intern(name);
    return hotReloadModule(ctx, module, nameInterned);
}

std::vector<ModuleInfo*> getLoadedModules(InterpreterContext& ctx) {
    return ctx.program
        ? ctx.program->registry().getAllModules()
        : std::vector<ModuleInfo*>{};
}

} // namespace interpreter