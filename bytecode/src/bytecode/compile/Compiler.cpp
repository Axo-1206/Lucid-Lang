/// @file bytecode/compile/Compiler.cpp
/// @brief The driver: walk a resolved module set, produce a Bytecode.

#include "bytecode/compile/Compiler.hpp"
#include "ArtifactBuildState.hpp"
#include "CompilerContext.hpp"
#include "LambdaLift.hpp"

#include "bytecode/emit/EmitDecl.hpp"
#include "bytecode/emit/EmitExpr.hpp"
#include "bytecode/emit/EmitStmt.hpp"
#include "bytecode/memory/DropSchedule.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"

using namespace lucid::contract;

namespace lucid::bytecode::compile {

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

Compiler::Compiler(lucid::diag::DiagnosticEngine& diagnostics,
                   StringPool& pool)
    : m_diag(diagnostics)
    , m_pool(pool) {}

// Defined here, where LambdaLift is a complete type. A default
// destructor in the header would require the unique_ptr's deleter to
// see the complete type at every Compiler destruction site.
Compiler::~Compiler() = default;

// ─────────────────────────────────────────────────────────────────────────────
// compile — the entry point
// ─────────────────────────────────────────────────────────────────────────────
//
// Two passes over the module set, with a lift stage between them:
//
//   Pass A — collect and bake. For each module, in source order:
//     1. Record its imports in the Manifest.
//     2. Call emitDeclArtifacts for every top-level declaration.
//        This bakes tables and top-level bindings into StaticData,
//        registers host symbols, and reserves function indices.
//     3. Record each declaration's mangled name in the Manifest's
//        per-module lists.
//
//   Lambda lift — synthesize a top-level function per lambda:
//     The lift reads each module's `lambdas` span (populated by
//     Sema), creates a FnDeclAST for each, and registers it. The
//     synthesized functions are emitted in pass B alongside the
//     module-level ones.
//
//   Pass B — emit bodies. For each module-level function, then each
//     synthesized lambda function:
//     1. Construct a CompilerContext.
//     2. Call emitDeclPrologue (parameter slots, opening line).
//     3. Emit the body:
//        - Module function: emitStmt on the body block, then an
//          implicit ReturnVoid unless the body transfers.
//        - Synthesized lambda: emitExpr on the body expression, mark
//          its ownership as moved, drop parameters, and Return.
//     4. Finalize the proto and replace the placeholder.

Bytecode Compiler::compile(const std::vector<ModuleAST*>& modules) {
    // ─── Preconditions ─────────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        AST_ASSERT_MSG(module != nullptr,
            "Compiler::compile: a null module was passed — the caller "
            "must filter nulls before invoking the compiler");
        AST_ASSERT_MSG(!module->hasErrors,
            "Compiler::compile: a module with Sema errors was passed — "
            "the caller must not invoke the compiler on a module whose "
            "Sema run failed");
    }

    // ─── Reset the per-compilation state ───────────────────────────────
    //
    // A Compiler may be reused. Clear the state that accumulates
    // across a compilation so a second call starts clean.
    m_functions.clear();
    m_functionIndex.clear();
    m_hostSymbolIndex.clear();
    m_staticDataOffsets.clear();
    m_tableIndices.clear();
    m_lambdaLift.reset();

    // ─── The artifact under construction ───────────────────────────────
    //
    // m_functions is a member (see the header). The other containers
    // are locals; nothing outside compile() needs them.
    Manifest        manifest;
    ConstantPool    constants;
    StaticData      staticData;
    HostSymbolTable hostSymbols;

    // ─── The shared pass-A state ───────────────────────────────────────
    ArtifactBuildState state{
        constants,
        staticData,
        hostSymbols,
        m_functions,
        m_pool,
        m_functionIndex,
        m_staticDataOffsets,
        m_tableIndices,
        m_hostSymbolIndex,
    };

    // ─── Pass A — collect and bake ─────────────────────────────────────
    for (ModuleAST* module : modules) {
        Manifest::Module modEntry;
        modEntry.modulePath = m_pool.lookup(module->filePath);

        // ─── Imports ───────────────────────────────────────────────────
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (!decl->isa<ImportDeclAST>()) continue;
            const auto* imp = decl->as<ImportDeclAST>();
            Manifest::Module::Import m;
            m.alias      = m_pool.lookup(imp->alias);
            m.targetPath = m_pool.lookup(imp->path);
            modEntry.imports.push_back(std::move(m));
        }

        // ─── Declarations ──────────────────────────────────────────────
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (decl->hasSyntaxError) continue;

            // Artifact-level parts: table, binding, function
            // registration, or host symbol. No code.
            emitDeclArtifacts(decl, state);

            // Manifest bookkeeping: record the declaration's mangled
            // name in the module's per-kind list. The manifest is the
            // driver's, not emitDeclArtifacts's.
            if (decl->isa<TableDeclAST>()) {
                modEntry.tables.push_back(
                    m_pool.lookup(decl->mangledName));
            } else if (decl->isa<FnDeclAST>()) {
                modEntry.functions.push_back(
                    m_pool.lookup(decl->mangledName));
            } else if (decl->isa<VarDeclAST>()) {
                modEntry.bindings.push_back(
                    m_pool.lookup(decl->mangledName));
            }
        }

        manifest.modules.push_back(std::move(modEntry));
    }

    // ─── Lambda lift ───────────────────────────────────────────────────
    //
    // Synthesize a top-level function for every lambda Sema collected
    // in every module. The lift reads each module's `lambdas` span
    // (Sema populated it during its own walk — the lift does not
    // re-walk the AST) and registers one function per lambda.
    //
    // The lift runs after pass A because a lambda's body may reference
    // module-level declarations whose artifact indices are assigned
    // in pass A. It runs before pass B because pass B emits every
    // function, including the synthesized ones.
    m_lambdaLift = std::make_unique<LambdaLift>(*this, m_pool);
    m_lambdaLift->lift(modules);

    // ─── Pass B — emit bodies ──────────────────────────────────────────
    //
    // First half: module-level functions.
    for (ModuleAST* module : modules) {
        for (DeclAST* decl : module->decls) {
            if (decl == nullptr) continue;
            if (!decl->isa<FnDeclAST>()) continue;

            auto* fn = decl->as<FnDeclAST>();
            if (fn->isHostBound) continue;

            auto it = m_functionIndex.find(fn);
            AST_ASSERT_MSG(it != m_functionIndex.end(),
                "Compiler::compile: a Lucid-bodied function was not "
                "registered in pass A — the driver's two passes are "
                "out of sync");

            CompilerContext ctx(constants, hostSymbols, staticData,
                                *this, module);
            ctx.setCurrentFn(fn);

            // Prologue: parameter slots (with resource kinds), the
            // function's opening line entry.
            emitDeclPrologue(fn, ctx);

            // Body. The flag tells us whether the body transfers
            // control out (a return somewhere in its statement
            // sequence). A body whose last reachable statement is a
            // return has already emitted Ext_Return or Ext_ReturnVoid
            // along with its own DropSchedule::emitReturnDrops call.
            const bool bodyTransfers = emitStmt(fn->body, ctx);

            // Implicit fall-through return. Only emit it if the body
            // did not already transfer control out.
            if (!bodyTransfers) {
                memory::DropSchedule::emitReturnDrops(ctx);
                ctx.emitOpcode(Opcode::Ext_ReturnVoid);
            }

            // Finalize: assemble the FunctionProto from the accumulated
            // code, line table, slot counts, resume table, and stack
            // depth. Replaces the pass-A placeholder.
            m_functions[it->second] = ctx.finalizeProto();
        }
    }

    // ─── Pass B, second half: synthesized lambda functions ─────────────
    //
    // A synthesized function's body is not a BlockStmtAST; it is the
    // lambda's body expression, held by the lift. The emitter handles
    // it directly: emit the expression, mark its ownership as moved
    // (the value is the return value), drop the parameters, and
    // return.
    for (FnDeclAST* fn : m_lambdaLift->synthesizedFunctions()) {
        auto it = m_functionIndex.find(fn);
        AST_ASSERT_MSG(it != m_functionIndex.end(),
            "Compiler::compile: a synthesized function was not "
            "registered — the lift's registration is out of sync");

        ModuleAST* owningModule = m_lambdaLift->moduleOf(fn);
        AST_ASSERT_MSG(owningModule != nullptr,
            "Compiler::compile: a synthesized function has no owning "
            "module — the lift's module map is out of sync");

        CompilerContext ctx(constants, hostSymbols, staticData,
                            *this, owningModule);
        ctx.setCurrentFn(fn);

        // The prologue: parameter slots and the opening line entry.
        // A synthesized function has no block body, but the prologue
        // does not require one — it allocates parameter slots and
        // records the opening line only.
        emitDeclPrologue(fn, ctx);

        // The body: the lambda's expression as the return value.
        ExprAST* bodyExpr = m_lambdaLift->bodyOf(fn);
        AST_ASSERT_MSG(bodyExpr != nullptr,
            "Compiler::compile: a synthesized function has no body "
            "expression — the lift's body map is out of sync");

        // Emit the expression. It leaves the return value on the
        // value stack.
        emitExpr(bodyExpr, ctx);

        // The return value's ownership transfers to the caller.
        // Mark its entry Moved so no drop below this stack position
        // touches it.
        ctx.owned().markTopAsMoved();

        // Drop the parameters (and any open scopes — a synthesized
        // function opens no scopes, so this is just the parameters).
        memory::DropSchedule::emitReturnDrops(ctx);

        // Return. Ext_Return pops the return value's ownership entry.
        ctx.emitOpcode(Opcode::Ext_Return);

        m_functions[it->second] = ctx.finalizeProto();
    }

    // ─── Assemble the artifact ─────────────────────────────────────────
    return Bytecode(std::move(manifest),
                    std::move(constants),
                    std::move(staticData),
                    std::move(hostSymbols),
                    std::move(m_functions));
}

// ─────────────────────────────────────────────────────────────────────────────
// Artifact index lookups
// ─────────────────────────────────────────────────────────────────────────────

std::optional<uint32_t> Compiler::functionIndexOf(
    const FnDeclAST* fn) const {
    auto it = m_functionIndex.find(fn);
    if (it == m_functionIndex.end()) return std::nullopt;
    return it->second;
}

std::optional<uint32_t> Compiler::staticDataOffsetOf(
    InternedString mangledName) const {
    auto it = m_staticDataOffsets.find(mangledName);
    if (it == m_staticDataOffsets.end()) return std::nullopt;
    return it->second;
}

std::optional<uint32_t> Compiler::tableIndexOf(
    InternedString mangledName) const {
    auto it = m_tableIndices.find(mangledName);
    if (it == m_tableIndices.end()) return std::nullopt;
    return it->second;
}

std::optional<uint32_t> Compiler::hostSymbolIndexOf(
    const FnDeclAST* fn) const {
    auto it = m_hostSymbolIndex.find(fn);
    if (it == m_hostSymbolIndex.end()) return std::nullopt;
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lambda-lift registration and lookup
// ─────────────────────────────────────────────────────────────────────────────

void Compiler::registerSynthesizedFunction(FnDeclAST* fn,
                                           SourceLocation loc) {
    AST_ASSERT_MSG(fn != nullptr,
        "Compiler::registerSynthesizedFunction: null function — the "
        "lift passed a null FnDeclAST");
    AST_ASSERT_MSG(fn->mangledName.isValid(),
        "Compiler::registerSynthesizedFunction: the function has no "
        "mangled name — the lift must set one before registering");

    // Reserve a FunctionProto index, the same way pass A reserves one
    // for a module-level function.
    const uint32_t index = static_cast<uint32_t>(m_functions.size());
    auto [it, inserted] = m_functionIndex.emplace(fn, index);
    AST_ASSERT_MSG(inserted,
        "Compiler::registerSynthesizedFunction: the function was "
        "already registered — the lift ran twice on the same lambda");

    // Placeholder proto: a minimal legal code stream. Pass B replaces
    // it with the real proto.
    std::vector<uint8_t> code;
    code.push_back(0x00);                                // escape
    code.push_back(opcodeStreamByte(Opcode::Ext_ReturnVoid));

    FunctionSignature sig;
    m_functions.push_back(FunctionProto(
        m_pool.lookup(fn->mangledName),
        std::move(sig),
        std::move(code),
        {},                         // no line table
        0,                          // no locals
        1,                          // maxStackDepth
        false,                      // a lambda is not a @sequence
        {}));                       // no resume table
    (void)loc;                      // reserved for future diagnostics
}

std::optional<uint32_t> Compiler::lambdaFunctionIndexFor(
    const LambdaExprAST* lambda) const {
    AST_ASSERT_MSG(lambda != nullptr,
        "Compiler::lambdaFunctionIndexFor: null lambda");
    if (m_lambdaLift == nullptr) {
        // The lift has not run. This happens only if a caller uses a
        // Compiler before its compile() call has passed the lift
        // stage; return nullopt and let the caller assert.
        return std::nullopt;
    }
    const FnDeclAST* fn = m_lambdaLift->functionFor(lambda);
    if (fn == nullptr) return std::nullopt;
    return functionIndexOf(fn);
}

} // namespace lucid::bytecode::compile